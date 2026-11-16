#define _GNU_SOURCE
#include "signal-router.h"
#include "signal-stack.h"
#include "nonlocal-mask.h"
#include "signal-deferred.h"
#include <dlfcn.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

extern void domain_signal_entry(int, siginfo_t *, void *);
/* Standalone registration-contract fixtures provide their own entry and have
 * no carrier registry. The full domain target supplies the mask translator. */
extern uint64_t signal_host_physical_mask(uint64_t) __attribute__((weak));
extern unsigned signal_deferred_recipients(void) __attribute__((weak));
extern int signal_mask_prepare(void) __attribute__((weak));
extern int signal_wait_prepare(void) __attribute__((weak));
extern int signal_suspend_prepare(void) __attribute__((weak));
extern int signal_fd_prepare(void) __attribute__((weak));
extern int signal_fd_channel_ready(void) __attribute__((weak));
extern int signal_fd_channel_busy(void) __attribute__((weak));
extern int signal_host_relay_running(void) __attribute__((weak));
extern int signal_host_relay_busy(void) __attribute__((weak));

struct disposition {
  struct sigaction action;
  uint64_t ignore_generation;
  _Atomic int fired;
  void *entry;
  size_t entry_size;
  struct disposition *next;
};

/* Records are immutable after publication. A signal may interrupt an updater
 * on any thread; no reader takes the writer lock or touches malloc. Retain
 * records until explicit recipient quiescence, including across siglongjmp.
 * Counting active readers after entry cannot cover a kernel-selected entry not
 * yet executed. Reclamation requires recipient quiescence, not just a zero
 * reader count. get_signal selects a disposition before ARM64 constructs the
 * user frame: https://github.com/torvalds/linux/blob/v6.12/kernel/signal.c
 * https://github.com/torvalds/linux/blob/v6.12/arch/arm64/kernel/signal.c */
static _Atomic(struct disposition *) current[NSIG];
static struct sigaction original[NSIG];
/* Baseline records may be captured by a helper frame. Each router lifetime
 * gets a fresh array; never overwrite a previous lifetime's saved token. The
 * arrays remain pinned, like other generations without a quiescence provider.
 */
static struct disposition *initial;
static unsigned char managed[NSIG];
static uint64_t host_reservations;
static _Atomic unsigned records, retained;
static struct disposition *allocations;
static atomic_flag writer = ATOMIC_FLAG_INIT;
static int (*system_action)(int, const struct sigaction *, struct sigaction *);
static _Atomic int enabled;
static _Atomic int guest_faults;
static int fault_notifications[GW_SIGNAL_FAULT_LANES];
static struct sigaction notification_original[GW_SIGNAL_FAULT_LANES];
static _Atomic int deferred_faults;
static uint64_t notification_initial_mask;
static long notification_owner;
static void lock_writer(uint64_t *);
static void unlock_writer(uint64_t);
static int make_entry(struct disposition *);
static int fault_lane(int);

static struct sigaction notification_action(struct disposition *record) {
  struct sigaction action = {
      .sa_sigaction = record->entry,
      .sa_flags =
          SA_SIGINFO | SA_ONSTACK | (record->action.sa_flags & SA_RESTART)};
  sigemptyset(&action.sa_mask);
  return action;
}

uint64_t signal_router_token_ignore_generation(void *token) {
  return ((struct disposition *)token)->ignore_generation;
}

const int *signal_router_fault_notifications(void) {
  return atomic_load_explicit(&deferred_faults, memory_order_acquire)
             ? fault_notifications
             : NULL;
}

void *signal_router_current_token(int number) {
  struct disposition *record =
      atomic_load_explicit(&current[number], memory_order_acquire);
  return record ? record : &initial[number];
}

int signal_router_defer_faults(const int *notifications) {
  if (!signal_router_enabled() || !signal_host_physical_mask) {
    return EINVAL;
  }
  uint64_t mask;
  lock_writer(&mask);
  if (atomic_load(&deferred_faults)) {
    unlock_writer(mask);
    return EALREADY;
  }
  if (signal_fd_channel_ready) {
    int error = signal_fd_channel_ready();
    if (error) {
      unlock_writer(mask);
      return error;
    }
  }
  uint64_t used = 0;
  for (int i = 0; i < GW_SIGNAL_FAULT_LANES; ++i) {
    int number = notifications[i];
    if (number < SIGRTMIN || number > SIGRTMAX ||
        (host_reservations & (UINT64_C(1) << (number - 1))) ||
        atomic_load_explicit(&current[number], memory_order_acquire) ||
        (used & (UINT64_C(1) << (number - 1)))) {
      unlock_writer(mask);
      return EINVAL;
    }
    used |= UINT64_C(1) << (number - 1);
    if (system_action(number, NULL, &notification_original[i])) {
      int error = errno;
      unlock_writer(mask);
      return error;
    }
    if (notification_original[i].sa_handler != SIG_DFL) {
      unlock_writer(mask);
      return EBUSY;
    }
  }
  const int faults[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGSYS};
  int installed = 0, result = 0;
  for (; installed < GW_SIGNAL_FAULT_LANES; ++installed) {
    struct disposition *record = signal_router_current_token(faults[installed]);
    if (!record->entry && make_entry(record)) {
      result = errno;
      break;
    }
    /* Bind both SA_RESTART and the logical action to the kernel-selected
     * immutable generation. Looking up current[] later could pair an old
     * interrupted-syscall decision with a newly registered handler. */
    struct sigaction action = notification_action(record);
    if (system_action(notifications[installed], &action, NULL)) {
      result = errno;
      break;
    }
  }
  if (result) {
    while (installed) {
      --installed;
      if (system_action(notifications[installed],
                        &notification_original[installed], NULL)) {
        _exit(105);
      }
    }
  } else {
    memcpy(fault_notifications, notifications, sizeof fault_notifications);
    for (int i = 0; i < GW_SIGNAL_FAULT_LANES; ++i) {
      /* Private entries have their own snapshot/verification/restore path. */
      managed[notifications[i]] = 0;
    }
    notification_initial_mask = mask & used;
    notification_owner = syscall(SYS_gettid);
    atomic_store_explicit(&deferred_faults, 1, memory_order_release);
  }
  unlock_writer(mask);
  return result;
}

static int fault_number(int number) {
  return number == SIGSEGV || number == SIGBUS || number == SIGILL ||
         number == SIGFPE || number == SIGTRAP || number == SIGSYS;
}

static int fault_lane(int number) {
  const int faults[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGSYS};
  for (int i = 0; i < GW_SIGNAL_FAULT_LANES; ++i) {
    if (number == faults[i]) {
      return i;
    }
  }
  return -1;
}

static int fault_routed(int number) {
  return atomic_load_explicit(&guest_faults, memory_order_acquire) &&
         fault_number(number);
}

static struct sigaction logical_action(struct disposition *record, int number) {
  struct sigaction action = record ? record->action : original[number];
  if (record && (action.sa_flags & SA_RESETHAND) &&
      atomic_load_explicit(&record->fired, memory_order_acquire)) {
    action.sa_handler = SIG_DFL;
  }
  return action;
}

uint64_t signal_router_ignore_generation(int number, int *ignored) {
  struct disposition *record =
      atomic_load_explicit(&current[number], memory_order_acquire);
  *ignored = logical_action(record, number).sa_handler == SIG_IGN;
  return record ? record->ignore_generation : 0;
}

static void lock_writer(uint64_t *previous) {
  uint64_t all = UINT64_MAX;
  /* Block all catchable signals before taking the lock. A handler cannot
   * interrupt this thread while it owns the lock and recursively deadlock. */
  if (syscall(SYS_rt_sigprocmask, SIG_SETMASK, &all, previous, 8)) {
    _exit(105);
  }
  while (atomic_flag_test_and_set_explicit(&writer, memory_order_acquire)) {
  }
}

static void unlock_writer(uint64_t previous) {
  atomic_flag_clear_explicit(&writer, memory_order_release);
  if (syscall(SYS_rt_sigprocmask, SIG_SETMASK, &previous, NULL, 8)) {
    _exit(105);
  }
}

uint64_t signal_router_selection_begin(void) {
  uint64_t mask;
  lock_writer(&mask);
  return mask;
}

void signal_router_selection_end(uint64_t mask) {
  unlock_writer(mask);
}

int signal_router_init_with_backend(signal_router_backend backend) {
  if (atomic_load(&enabled)) {
    return EALREADY;
  }
  system_action = backend;
  if (!system_action) {
    return ENOSYS;
  }
  /* The fixture covers HotSpot's default SR signal only. Do not silently
   * allow an overridden signal to bypass TLS restoration or Guest ownership. */
  if (getenv("_JAVA_SR_SIGNUM")) {
    return ENOTSUP;
  }
  /* Resolve optional interposer backends before any route can enter a Host
   * handler. sigprocmask and sigsuspend must not first call dlsym there. */
  int (*prepare[])(void) = {signal_mask_prepare, signal_wait_prepare,
                            signal_suspend_prepare, signal_fd_prepare};
  int saved_errno = errno;
  for (unsigned i = 0; i < sizeof prepare / sizeof prepare[0]; ++i) {
    int error = prepare[i] ? prepare[i]() : 0;
    if (error) {
      errno = saved_errno;
      return error;
    }
  }
  errno = saved_errno;
  const int reserved[] = {SIGSEGV, SIGBUS,  SIGILL,  SIGFPE, SIGTRAP, SIGSYS,
                          SIGPIPE, SIGXFSZ, SIGUSR2, SIGHUP, SIGINT,  SIGTERM,
                          SIGQUIT, 50,      51,      52,     53};
  host_reservations = 0;
  for (unsigned i = 0; i < sizeof reserved / sizeof reserved[0]; ++i) {
    host_reservations |= UINT64_C(1) << (reserved[i] - 1);
  }
  /* Routing a Host registration is not an exclusive VM reservation. Untagged
   * external packets belong to Host on every public signal, including USR1,
   * ALRM, CHLD and unreserved RT numbers. Keep native libc's private numbers
   * outside this public layer. The explicit late adapter separately adopts
   * pre-existing handlers, with its reviewed outer-chain requirement. */
  memset(managed, 0, sizeof managed);
  for (int number = 1; number < NSIG; ++number) {
    if (number == SIGKILL || number == SIGSTOP ||
        (number >= 32 && number < SIGRTMIN)) {
      continue;
    }
    if (system_action(number, NULL, &original[number])) {
      return errno;
    }
    atomic_store(&current[number], NULL);
    managed[number] = 1;
  }
  struct disposition *baseline =
      mmap(NULL, sizeof *baseline * NSIG, PROT_READ | PROT_WRITE,
           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (baseline == MAP_FAILED) {
    return errno;
  }
  for (int number = 1; number < NSIG; ++number) {
    baseline[number].action = original[number];
  }
  initial = baseline;
  atomic_store_explicit(&enabled, 1, memory_order_release);
  return 0;
}

int signal_router_init(void) {
  return signal_router_init_with_backend(dlsym(RTLD_NEXT, "sigaction"));
}

static int same_action(const struct sigaction *left,
                       const struct sigaction *right) {
  if (left->sa_handler != right->sa_handler ||
      left->sa_flags != right->sa_flags) {
    return 0;
  }
  for (int number = 1; number < NSIG; ++number) {
    if (sigismember(&left->sa_mask, number) !=
        sigismember(&right->sa_mask, number)) {
      return 0;
    }
  }
  return 1;
}

/* Only for an explicit late-loading integration, before enabling any Guest
 * carrier. The integrator must serialize Host registration during adoption.
 * Deliveries remain safe: immutable entries are complete before installation.
 * A layered backend cannot establish that its outer handler restores TLS. */
int signal_router_adopt_chain(void *reviewed_chain_base) {
  if (!atomic_load_explicit(&enabled, memory_order_acquire)) {
    return EINVAL;
  }
  for (int number = 1; number < NSIG; ++number) {
    if (!managed[number]) {
      continue;
    }
    struct sigaction actual;
    if (system_action(number, NULL, &actual)) {
      return errno;
    }
    if (!same_action(&actual, &original[number]) ||
        atomic_load(&current[number])) {
      return EBUSY;
    }
    Dl_info owner = {0};
    if (actual.sa_handler != SIG_DFL && actual.sa_handler != SIG_IGN &&
        dladdr((void *)actual.sa_sigaction, &owner) && owner.dli_fname &&
        (strstr(owner.dli_fname, "libsigchain.so") ||
         strstr(owner.dli_fname, "libart.so")) &&
        owner.dli_fbase != reviewed_chain_base) {
      /* ART reads pthread TLS before its special handlers. Wrapping only the
       * user handler is too late; do not mutate this chain speculatively. */
      return ENOTSUP;
    }
  }
  for (int number = 1; number < NSIG; ++number) {
    if (managed[number] && original[number].sa_handler != SIG_DFL &&
        original[number].sa_handler != SIG_IGN &&
        signal_router_action(number, &original[number], NULL)) {
      /* The transaction owner restores all layers exactly once. Partial
       * adoption retains immutable entries until process disposal. */
      return errno;
    }
  }
  return 0;
}

int signal_router_adopt(void) {
  return signal_router_adopt_chain(NULL);
}

/* A distinct immutable entry binds the kernel action (including its mask
 * and flags) to the corresponding logical handler. Looking up current[] at
 * delivery time would mix an old kernel frame with a newer registration. */
static int make_entry(struct disposition *record) {
  size_t page = (size_t)sysconf(_SC_PAGESIZE);
  unsigned char *code = mmap(NULL, page, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (code == MAP_FAILED) {
    return -1;
  }
  uintptr_t token = (uintptr_t)record, target = (uintptr_t)domain_signal_entry;
#if defined(__aarch64__)
  const uint32_t instructions[] = {0xd503245f, 0x58000063, 0x58000090,
                                   0xd61f0200};
  memcpy(code, instructions, sizeof instructions);
  memcpy(code + 16, &token, 8);
  memcpy(code + 24, &target, 8);
#else
  const unsigned char instructions[] = {
      0xf3, 0x0f, 0x1e, 0xfa, 0x48, 0xb9, 0, 0, 0, 0, 0,    0,    0,   0,
      0x49, 0xbb, 0,    0,    0,    0,    0, 0, 0, 0, 0x41, 0xff, 0xe3};
  memcpy(code, instructions, sizeof instructions);
  memcpy(code + 6, &token, 8);
  memcpy(code + 16, &target, 8);
#endif
  __builtin___clear_cache((char *)code, (char *)code + 32);
  if (mprotect(code, page, PROT_READ | PROT_EXEC)) {
    int error = errno;
    munmap(code, page);
    errno = error;
    return -1;
  }
  record->entry = code;
  record->entry_size = page;
  return 0;
}

/* Exported by the experiment executable so libjvm's public sigaction calls
 * enter here. Queries return the logical disposition, not the routing entry.
 * Explicit dlsym(libc, ...) and raw syscalls are outside this interception. */
int signal_router_action(int number, const struct sigaction *action,
                         struct sigaction *previous) {
  if (!system_action) {
    errno = ENOSYS;
    return -1;
  }
  const int *notifications = signal_router_fault_notifications();
  if (notifications) {
    for (int i = 0; i < GW_SIGNAL_FAULT_LANES; ++i) {
      if (number == notifications[i]) {
        errno = EBUSY;
        return -1;
      }
    }
  }
  if (!atomic_load_explicit(&enabled, memory_order_acquire) || number <= 0 ||
      number >= NSIG || !managed[number]) {
    return system_action(number, action, previous);
  }
  uint64_t mask;
  lock_writer(&mask);
  struct disposition *old =
      atomic_load_explicit(&current[number], memory_order_acquire);
  if (!action) {
    struct sigaction actual;
    int status = system_action(number, NULL, &actual);
    int error = errno;
    if (!status && previous) {
      *previous = logical_action(old, number);
      /* Preserve kernel feature probing (notably ART's SA_UNSUPPORTED).
       * Hide our synthetic SA_SIGINFO while removing request bits that the
       * kernel did not accept. Immutable records remain safe for delivery. */
      previous->sa_flags &= actual.sa_flags;
      if (fault_routed(number)) {
        /* The outer handler never resets on a Guest delivery. These are
         * logical Host flags, not a probe of that routing implementation. */
        previous->sa_flags =
            (old ? old->action.sa_flags : original[number].sa_flags) &
            (actual.sa_flags | SA_RESETHAND);
      }
      /* SA_RESETHAND resets in the kernel before our entry can run. The
       * carrier may be descheduled in that window; fired is not authoritative.
       */
      previous->sa_handler =
          actual.sa_handler == SIG_DFL || actual.sa_handler == SIG_IGN
              ? actual.sa_handler
              : previous->sa_handler;
    }
    unlock_writer(mask);
    errno = error;
    return status;
  }
  /* Immutable generations remain valid until an explicit recipient-quiescence
   * gate permits reclamation. Never allocate/free through a foreign libc. */
  struct disposition *next = mmap(NULL, sizeof *next, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (next == MAP_FAILED) {
    int error = errno;
    unlock_writer(mask);
    errno = error;
    return -1;
  }
  next->action = *action;
  next->ignore_generation = old ? old->ignore_generation : 0;
  if (action->sa_handler == SIG_IGN) {
    if (next->ignore_generation == UINT64_MAX) {
      munmap(next, sizeof *next);
      unlock_writer(mask);
      errno = EOVERFLOW;
      return -1;
    }
    ++next->ignore_generation;
  }
  struct sigaction installed = *action;
  int lane = notifications ? fault_lane(number) : -1;
  if (lane >= 0 || fault_routed(number) ||
      (action->sa_handler != SIG_DFL && action->sa_handler != SIG_IGN)) {
    if (make_entry(next)) {
      int error = errno;
      munmap(next, sizeof *next);
      unlock_writer(mask);
      errno = error;
      return -1;
    }
    if (fault_routed(number) ||
        (action->sa_handler != SIG_DFL && action->sa_handler != SIG_IGN)) {
      installed.sa_sigaction = next->entry;
      installed.sa_flags |= SA_SIGINFO;
    }
    if (fault_routed(number)) {
      /* Kernel SA_RESETHAND would consume Host's one-shot on a Guest fault.
       * Consume it atomically only when forwarding to the Host handler.
       * https://github.com/torvalds/linux/blob/v6.12/kernel/signal.c */
      installed.sa_flags &= ~SA_RESETHAND;
      installed.sa_flags |= SA_ONSTACK;
    }
  }
  struct sigaction replaced;
  int status = system_action(number, &installed, &replaced);
  int error = errno;
  int exposed = !status && installed.sa_sigaction == next->entry;
  if (!status && lane >= 0) {
    struct sigaction helper = notification_action(next);
    if (system_action(notifications[lane], &helper, NULL)) {
      error = errno;
      if (system_action(number, &replaced, NULL)) {
        _exit(106);
      }
      status = -1;
    }
  }
  if (!status) {
    next->next = allocations;
    allocations = next;
    atomic_fetch_add_explicit(&retained, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&records, 1, memory_order_relaxed);
    atomic_store_explicit(&current[number], next, memory_order_release);
    /* Retain a consumed one-shot's flags/mask. The kernel resets only its
     * handler; replacing the record with an empty SIG_DFL action would change
     * subsequent queries and the previous action returned by registration.
     * https://github.com/torvalds/linux/blob/v6.12/kernel/signal.c */
    if (previous) {
      *previous = logical_action(old, number);
      unsigned logical_flags = previous->sa_flags;
      previous->sa_flags &= replaced.sa_flags;
      if (fault_routed(number)) {
        previous->sa_flags = logical_flags & (replaced.sa_flags | SA_RESETHAND);
      }
      if (replaced.sa_handler == SIG_DFL || replaced.sa_handler == SIG_IGN) {
        previous->sa_handler = replaced.sa_handler;
      }
    }
  } else {
    if (exposed) {
      /* A kernel frame can retain the transient first installation even after
       * rollback. Pin it until the same explicit quiescence as other records.
       */
      next->next = allocations;
      allocations = next;
      atomic_fetch_add_explicit(&retained, 1, memory_order_relaxed);
    } else {
      if (next->entry) {
        munmap(next->entry, next->entry_size);
      }
      munmap(next, sizeof *next);
    }
  }
  unlock_writer(mask);
  errno = error;
  return status;
}

int signal_router_guest_faults(void) {
  if (!signal_router_enabled()) {
    return EINVAL;
  }
  if (atomic_exchange_explicit(&guest_faults, 1, memory_order_acq_rel)) {
    return EALREADY;
  }
  for (int number = 1; number < NSIG; ++number) {
    if (!fault_number(number)) {
      continue;
    }
    struct sigaction action;
    if (signal_router_action(number, NULL, &action) ||
        signal_router_action(number, &action, NULL)) {
      return errno;
    }
  }
  return 0;
}

#ifdef __ANDROID__
/* This experiment builds only LP64 Android. Convert by copying rather than
 * aliasing two distinct struct types through incompatible pointers. */
int signal_router_action64(int number, const struct sigaction64 *action,
                           struct sigaction64 *previous) {
  _Static_assert(sizeof(struct sigaction64) == sizeof(struct sigaction),
                 "LP64 signal ABI required");
  struct sigaction input, output;
  if (action) {
    memcpy(&input, action, sizeof input);
  }
  int result = signal_router_action(number, action ? &input : NULL,
                                    previous ? &output : NULL);
  if (!result && previous) {
    memcpy(previous, &output, sizeof output);
  }
  return result;
}

#endif

int signal_router_select(int number, const siginfo_t *info, void *token) {
  struct disposition *record = token;
  if (!record) {
    /* A concurrently replaced disposition cannot be guessed safely. */
    _exit(106);
  }
  struct sigaction action = record->action;
  if (action.sa_handler == SIG_IGN) {
    /* The outer route hides SIG_IGN from Linux. Forced synchronous faults
     * must still reset an ignored disposition and terminate, rather than
     * retrying the fault forever. This rule does not apply to async packets.
     * https://github.com/torvalds/linux/blob/v6.12/kernel/signal.c
     * See force_sig_info_to_task(). */
    if (info->si_code > 0 && fault_number(number)) {
      return 2;
    }
    return 0;
  }
  if (action.sa_handler == SIG_DFL) {
    return 2;
  }
  if (action.sa_flags & SA_RESETHAND) {
    if (fault_routed(number)) {
      if (atomic_exchange_explicit(&record->fired, 1, memory_order_acq_rel)) {
        return 2;
      }
    } else {
      atomic_store_explicit(&record->fired, 1, memory_order_release);
      /* Mutate only this generation, never a newer concurrent registration.
       * Keep the rest of the consumed kernel disposition observable. */
    }
  }
  return 1;
}

static void invoke_delivery(int number, siginfo_t *info, void *context,
                            const struct disposition *record) {
  int saved_errno = errno;
  const struct sigaction *action = &record->action;
  if (action->sa_flags & SA_SIGINFO) {
    action->sa_sigaction(number, info, context);
  } else {
    action->sa_handler(number);
  }
  errno = saved_errno;
}

void signal_router_forward(int number, siginfo_t *info, void *context,
                           void *token) {
  int selected = signal_router_select(number, info, token);
  if (selected == 2) {
    signal_router_default(number);
  }
  if (selected) {
    invoke_delivery(number, info, context, token);
  }
}

struct host_delivery {
  int number;
  siginfo_t *information;
  void *context, *token;
  uint64_t mask;
  const stack_t *stack;
};

static void host_delivery_run(void *argument) {
  struct host_delivery *delivery = argument;
  if (delivery->stack) {
    gw_signal_stack_set(delivery->stack);
  }
  gw_nonlocal_mask_restore(signal_host_physical_mask
                               ? signal_host_physical_mask(delivery->mask)
                               : delivery->mask);
  extern void signal_router_delivery_observe(int) __attribute__((weak));
  if (signal_router_delivery_observe) {
    signal_router_delivery_observe(delivery->number);
  }
  invoke_delivery(delivery->number, delivery->information, delivery->context,
                  delivery->token);
  gw_nonlocal_mask_block();
}

void signal_router_forward_domain(int number, siginfo_t *info, void *context,
                                  void *token, uint64_t host_mask,
                                  const stack_t *host_stack, uintptr_t host_sp,
                                  int foreign) {
  signal_router_forward_selected_domain(number, info, context, token, host_mask,
                                        host_stack, host_sp, foreign, -1);
}

void signal_router_forward_selected_domain(
    int number, siginfo_t *info, void *context, void *token, uint64_t host_mask,
    const stack_t *host_stack, uintptr_t host_sp, int foreign, int selected) {
  struct disposition *record = token;
  if (!record) {
    _exit(106);
  }
  uint64_t original_mask = gw_nonlocal_mask_block(), mask;
  if (selected < 0) {
    selected = signal_router_select(number, info, token);
  }
  if (selected == 2) {
    signal_router_default(number);
  }
  if (!selected) {
    gw_nonlocal_mask_restore(original_mask);
    return;
  }
  memcpy(&mask, &record->action.sa_mask, sizeof mask);
  mask |= host_mask;
  if (!(record->action.sa_flags & SA_NODEFER)) {
    mask |= UINT64_C(1) << (number - 1);
  }
  uintptr_t top = 0;
  stack_t physical, effective = *host_stack;
  int configure = foreign;
  if (foreign) {
    gw_signal_stack_get(&physical);
    if ((record->action.sa_flags & SA_ONSTACK) &&
        !(host_stack->ss_flags & SS_DISABLE) &&
        !gw_stack_contains(host_stack, host_sp)) {
      top = (uintptr_t)host_stack->ss_sp + host_stack->ss_size;
      if ((unsigned)host_stack->ss_flags & GW_SS_AUTODISARM) {
        effective = (stack_t){.ss_flags = SS_DISABLE};
      }
    } else {
      top = gw_signal_stack_top(host_sp);
    }
  } else {
    /* The outer fault route always requests a kernel alternate frame so a
     * Guest action can independently select SA_ONSTACK. Correct a Host action
     * which requested the ordinary interrupted stack instead.
     * https://github.com/torvalds/linux/blob/v6.12/kernel/signal.c */
    uintptr_t here = (uintptr_t)&original_mask;
    if (!(record->action.sa_flags & SA_ONSTACK) &&
        gw_stack_contains(host_stack, here) &&
        !gw_stack_contains(host_stack, host_sp)) {
      top = gw_signal_stack_top(host_sp);
      if ((unsigned)host_stack->ss_flags & GW_SS_AUTODISARM) {
        /* Undo the outer route's synthetic alternate-stack selection while
         * the logical Host action runs on its ordinary stack. */
        configure = 1;
        gw_signal_stack_get(&physical);
      }
    }
  }
  struct host_delivery delivery = {.number = number,
                                   .information = info,
                                   .context = context,
                                   .token = token,
                                   .mask = mask,
                                   .stack = configure ? &effective : NULL};
  domain_stack_call(host_delivery_run, &delivery, top);
  if (configure) {
    gw_signal_stack_set(&physical);
  }
  gw_nonlocal_mask_restore(original_mask);
}

_Noreturn void signal_router_default(int number) {
  struct sigaction action = {.sa_handler = SIG_DFL};
  sigemptyset(&action.sa_mask);
  if (system_action(number, &action, NULL)) {
    _exit(106);
  }
  uint64_t bit = UINT64_C(1) << (number - 1);
  if (syscall(SYS_rt_sigprocmask, SIG_UNBLOCK, &bit, NULL, sizeof bit) ||
      syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), number)) {
    _exit(106);
  }
  _exit(128 + number);
}

int signal_router_verify(void) {
  const int *notifications = signal_router_fault_notifications();
  if (notifications) {
    for (int i = 0; i < GW_SIGNAL_FAULT_LANES; ++i) {
      struct sigaction action;
      if (system_action(notifications[i], NULL, &action)) {
        return errno;
      }
      const int faults[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGSYS};
      struct disposition *record = signal_router_current_token(faults[i]);
      if (action.sa_sigaction != record->entry ||
          !(action.sa_flags & SA_SIGINFO) || (action.sa_flags & SA_RESETHAND) ||
          ((action.sa_flags ^ record->action.sa_flags) & SA_RESTART)) {
        return EBUSY;
      }
    }
  }
  for (int number = 1; number < NSIG; ++number) {
    struct disposition *record =
        atomic_load_explicit(&current[number], memory_order_acquire);
    if (!managed[number]) {
      continue;
    }
    struct sigaction actual;
    if (system_action(number, NULL, &actual)) {
      return errno;
    }
    if (!record) {
      if (!same_action(&actual, &original[number])) {
        return EBUSY;
      }
      continue;
    }
    /* The kernel can consume SA_RESETHAND before entering user code, hence
     * before fired is published. SIG_DFL with the unchanged flags/mask is a
     * valid consumed generation during that interval too. */
    int consumed = !fault_routed(number) &&
                   (record->action.sa_flags & SA_RESETHAND) &&
                   actual.sa_handler == SIG_DFL;
    int custom = fault_routed(number) ||
                 (!consumed && record->action.sa_handler != SIG_DFL &&
                  record->action.sa_handler != SIG_IGN);
    if (custom ? (void *)actual.sa_sigaction != record->entry
               : actual.sa_handler !=
                     (consumed ? SIG_DFL : record->action.sa_handler)) {
      return EBUSY;
    }
    if (record->entry && (custom || consumed)) {
      const unsigned flags = SA_RESTART | SA_ONSTACK | SA_NODEFER |
                             SA_RESETHAND | SA_SIGINFO | SA_NOCLDSTOP |
                             SA_NOCLDWAIT;
      unsigned expected_flags = (unsigned)record->action.sa_flags | SA_SIGINFO;
      if (fault_routed(number)) {
        expected_flags &= ~SA_RESETHAND;
        expected_flags |= SA_ONSTACK;
      }
      if (((unsigned)actual.sa_flags & flags) != (expected_flags & flags)) {
        return EBUSY;
      }
      for (int blocked = 1; blocked < NSIG; ++blocked) {
        if (blocked != SIGKILL && blocked != SIGSTOP &&
            sigismember(&actual.sa_mask, blocked) !=
                sigismember(&record->action.sa_mask, blocked)) {
          return EBUSY;
        }
      }
    }
  }
  return 0;
}

int signal_router_guest_ready(void) {
  if (!signal_router_enabled()) {
    return EINVAL;
  }
  int error = signal_router_verify();
  if (error) {
    return error;
  }
  for (int number = 1; number < NSIG; ++number) {
    if (managed[number] &&
        !atomic_load_explicit(&current[number], memory_order_acquire) &&
        original[number].sa_handler != SIG_DFL &&
        original[number].sa_handler != SIG_IGN) {
      return ENOTSUP;
    }
  }
  return 0;
}

int signal_router_restore(void) {
  if (signal_host_relay_running && signal_host_relay_running()) {
    return EBUSY;
  }
  if (signal_fd_channel_busy) {
    int error = signal_fd_channel_busy();
    if (error) {
      return error;
    }
  }
  /* Caller has joined all test carriers and destroyed the VM. */
  if (signal_router_fault_notifications() && signal_deferred_recipients &&
      signal_deferred_recipients()) {
    return EBUSY;
  }
  const int *notifications = signal_router_fault_notifications();
  if (notifications && notification_owner != syscall(SYS_gettid)) {
    return EBUSY;
  }
  uint64_t previous = gw_nonlocal_mask_block(), notification_bits = 0;
  int error = signal_router_verify();
  if (error) {
    gw_nonlocal_mask_restore(previous);
    return error;  // Do not overwrite a handler installed outside this router.
  }
  for (int number = 1; number < NSIG; ++number) {
    if (managed[number] && system_action(number, &original[number], NULL)) {
      error = errno;
      gw_nonlocal_mask_restore(previous);
      return error;
    }
  }
  if (notifications) {
    for (int i = 0; i < GW_SIGNAL_FAULT_LANES; ++i) {
      if (system_action(notifications[i], &notification_original[i], NULL)) {
        error = errno;
        gw_nonlocal_mask_restore(previous);
        return error;
      }
      notification_bits |= UINT64_C(1) << (notifications[i] - 1);
    }
    atomic_store_explicit(&deferred_faults, 0, memory_order_release);
  }
  atomic_store(&enabled, 0);
  atomic_store(&guest_faults, 0);
  /* Private mask bits must not leak into the Host mask after disabling the
   * translator. Other Host mask changes made during runtime use are retained.
   * The integration owner has joined the remaining recipients before restore.
   */
  if (notifications) {
    previous = (previous & ~notification_bits) | notification_initial_mask;
  }
  gw_nonlocal_mask_restore(previous);
  return 0;
}

unsigned signal_router_registrations(void) {
  return atomic_load(&records);
}

uint64_t signal_router_reserved(void) {
  uint64_t mask = host_reservations;
  const int *notifications = signal_router_fault_notifications();
  if (notifications) {
    for (int i = 0; i < GW_SIGNAL_FAULT_LANES; ++i) {
      mask |= UINT64_C(1) << (notifications[i] - 1);
    }
  }
  return mask;
}

int signal_router_enabled(void) {
  return atomic_load_explicit(&enabled, memory_order_acquire);
}

unsigned signal_router_retained(void) {
  return atomic_load_explicit(&retained, memory_order_relaxed);
}

int signal_router_reclaim(const struct signal_router_quiescence *gate,
                          unsigned *reclaimed) {
  if (!gate || gate->version != GW_SIGNAL_ROUTER_QUIESCENCE_V1 ||
      gate->size != sizeof *gate || !gate->enter || !gate->leave ||
      !reclaimed) {
    return EINVAL;
  }
  uint64_t mask;
  lock_writer(&mask);
  int error = gate->enter(gate->context);
  if (error) {
    unlock_writer(mask);
    return error;
  }
  if (signal_host_relay_busy && signal_host_relay_busy()) {
    gate->leave(gate->context);
    unlock_writer(mask);
    return EBUSY;
  }
  unsigned count = 0;
  int live = atomic_load_explicit(&enabled, memory_order_acquire);
  struct disposition **link = &allocations;
  while (*link) {
    struct disposition *record = *link;
    int published = 0;
    if (live) {
      for (int number = 1; number < NSIG; ++number) {
        if (atomic_load_explicit(&current[number], memory_order_acquire) ==
            record) {
          published = 1;
          break;
        }
      }
    }
    if (published) {
      link = &record->next;
      continue;
    }
    *link = record->next;
    if (record->entry) {
      munmap(record->entry, record->entry_size);
    }
    munmap(record, sizeof *record);
    ++count;
  }
  if (!live) {
    for (int number = 1; number < NSIG; ++number) {
      atomic_store_explicit(&current[number], NULL, memory_order_relaxed);
    }
  }
  atomic_fetch_sub_explicit(&retained, count, memory_order_relaxed);
  *reclaimed = count;
  gate->leave(gate->context);
  unlock_writer(mask);
  return 0;
}
