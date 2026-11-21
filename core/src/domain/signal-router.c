#define _GNU_SOURCE
#include "signal-router.h"
#include "signal-stack.h"
#include "nonlocal-mask.h"
#include "signal-deferred.h"
#include "signal-quiescence.h"
#include "signal_channel.h"
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
extern char domain_signal_masked[] __attribute__((weak));
#if defined(__aarch64__)
extern char domain_signal_token_saved[] __attribute__((weak));
#endif
/* Standalone registration-contract fixtures provide their own entry and have
 * no carrier registry. The full domain target supplies the mask translator. */
extern uint64_t signal_host_physical_mask(uint64_t) __attribute__((weak));
extern unsigned signal_deferred_recipients(void) __attribute__((weak));
extern int signal_mask_prepare(void) __attribute__((weak));
extern int signal_wait_prepare(void) __attribute__((weak));
extern int signal_suspend_prepare(void) __attribute__((weak));
extern int signal_fd_prepare(void) __attribute__((weak));
extern int signal_sleep_prepare(void) __attribute__((weak));
extern int gw_signal_user_copy_prepare(void) __attribute__((weak));
extern int signal_fd_channel_ready(void) __attribute__((weak));
extern int signal_fd_channel_busy(void) __attribute__((weak));
extern int signal_host_relay_active(void) __attribute__((weak));
extern int signal_host_relay_busy(void) __attribute__((weak));
extern int signal_stdio_enable(void) __attribute__((weak));
extern int signal_stdio_verify(void) __attribute__((weak));
extern int signal_stdio_disable(void) __attribute__((weak));
extern uint64_t gw_signal_wait_entry_mask(const ucontext_t *, uint64_t)
    __attribute__((weak));

struct disposition {
  struct sigaction action;
  uint64_t ignore_generation;
  uint64_t prefix_mask;
  _Atomic int fired;
  int internal, guarded, ordinary, exposed_context;
  void *entry;
  size_t entry_size;
  struct disposition *next;
};

/* A kernel frame can reference an immutable generation before its first
 * instruction executes. A fully masked entry excludes the probe until it
 * binds. Ordinary entries rely on inspecting the interrupted PC and saving
 * generations that a nested handler can expose to another continuation.
 * https://github.com/torvalds/linux/blob/v6.12/kernel/signal.c
 * https://github.com/torvalds/linux/blob/v6.12/arch/arm64/kernel/signal.c */
static _Atomic(struct disposition *) current[NSIG];
/* The low bit is the ignored disposition. Pending readers need a value
 * snapshot, without borrowing a pointer from current[]. */
static _Atomic uint64_t ignore_state[NSIG];
static struct sigaction original[NSIG];
/* Baseline records may be captured by a helper frame. Each router lifetime
 * gets a fresh array; never overwrite a previous lifetime's saved token. The
 * arrays remain pinned, like other generations without a quiescence provider.
 */
static struct disposition *initial;
static unsigned char managed[NSIG];
static uint64_t host_reservations;
static _Atomic uint64_t guest_private;
static _Atomic unsigned records, retained;
static struct disposition *allocations;
static struct disposition *reclaim_batch;
static _Atomic int routing_complete;
static atomic_flag writer = ATOMIC_FLAG_INIT;
static atomic_flag reclaimer = ATOMIC_FLAG_INIT;
static _Atomic int reclaim_signal;
static int reclaim_process;
static _Atomic(signal_router_backend) system_action;
static _Atomic int enabled;
static _Atomic int guest_faults;
static int fault_notifications[GW_SIGNAL_FAULT_LANES];
static struct sigaction notification_original[GW_SIGNAL_FAULT_LANES];
static _Atomic int deferred_faults, damaged;
static uint64_t notification_initial_mask;
static long notification_owner;
#ifdef __GLIBC__
struct kernel_action {
  uintptr_t handler, flags, restorer;
  uint64_t mask;
};

static struct kernel_action native_setxid, routed_setxid;
static _Atomic int host_setxid_active;
#endif
static void lock_writer(uint64_t *);
static void unlock_writer(uint64_t);
static int make_entry(struct disposition *);
static int fault_lane(int);
static int fault_routed(int);
static int widen_host_fault_routes(void);
static int same_action(const struct sigaction *, const struct sigaction *);

static uint64_t native_prefix_mask(void) {
  uint64_t mask = atomic_load_explicit(&guest_private, memory_order_acquire);
#ifdef __GLIBC__
  mask |= UINT64_C(3) << (GW_GNU_HOST_CANCEL - 1);
#endif
  return mask;
}

static void wide_fault_mask(struct sigaction *action) {
  uint64_t all = UINT64_MAX;
  sigemptyset(&action->sa_mask);
  memcpy(&action->sa_mask, &all, sizeof all);
}

static int kernel_mask_is_full(const struct sigaction *action) {
  uint64_t mask;
  memcpy(&mask, &action->sa_mask, sizeof mask);
  const uint64_t uncatchable =
      (UINT64_C(1) << (SIGKILL - 1)) | (UINT64_C(1) << (SIGSTOP - 1));
  return (mask | uncatchable) == UINT64_MAX;
}

static struct sigaction notification_action(struct disposition *record) {
  struct sigaction action = {
      .sa_sigaction = record->entry,
      .sa_flags =
          SA_SIGINFO | SA_ONSTACK | (record->action.sa_flags & SA_RESTART)};
  wide_fault_mask(&action);
  return action;
}

uint64_t signal_router_token_ignore_generation(void *token) {
  return ((struct disposition *)token)->ignore_generation;
}

__attribute__((no_stack_protector)) int signal_router_quarantine(void) {
  atomic_store_explicit(&damaged, 1, memory_order_release);
  return EIO;
}

int signal_router_error(void) {
  return atomic_load_explicit(&damaged, memory_order_acquire) ? EIO : 0;
}

int signal_router_token_restart(void *token) {
  struct disposition *record = token;
  return record->action.sa_handler == SIG_IGN ||
         !!(record->action.sa_flags & SA_RESTART);
}

const int *signal_router_fault_notifications(void) {
  return atomic_load_explicit(&deferred_faults, memory_order_acquire)
             ? fault_notifications
             : NULL;
}

static struct disposition *current_token(int number) {
  struct disposition *record =
      atomic_load_explicit(&current[number], memory_order_acquire);
  return record ? record : &initial[number];
}

int signal_router_defer_faults(const int *notifications) {
  if (signal_router_error()) {
    return EIO;
  }

  if (!signal_router_enabled() || !signal_host_physical_mask) {
    return EINVAL;
  }

  uint64_t mask;
  lock_writer(&mask);
  if (!signal_router_enabled() || signal_router_error()) {
    int error = signal_router_error();
    unlock_writer(mask);
    return error ? error : EINVAL;
  }
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
        (signal_router_reserved() & (UINT64_C(1) << (number - 1))) ||
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

  if (gw_signal_user_copy_prepare) {
    int error = gw_signal_user_copy_prepare();
    if (error) {
      unlock_writer(mask);
      return error;
    }
  }

  if (signal_stdio_enable) {
    int error = signal_stdio_enable();
    if (error) {
      if (error == EIO) {
        atomic_store_explicit(&damaged, 1, memory_order_release);
      }
      unlock_writer(mask);
      return error;
    }
  }

  const int faults[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGSYS};
  int installed = 0, result = 0;
  for (; installed < GW_SIGNAL_FAULT_LANES; ++installed) {
    struct disposition *record = current_token(faults[installed]);
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

  if (!result) {
    result = widen_host_fault_routes();
  }

  if (result) {
    while (installed) {
      --installed;
      if (system_action(notifications[installed],
                        &notification_original[installed], NULL)) {
        atomic_store_explicit(&damaged, 1, memory_order_release);
        result = EIO;
      }
    }

    if (signal_stdio_disable && signal_stdio_disable()) {
      /* Stdio callbacks remain mapped. A failed rollback is terminal for
       * this runtime; the owner disposes of the quarantined process. */
      atomic_store_explicit(&damaged, 1, memory_order_release);
      result = EIO;
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

/* Bootstrap owns registration/recipient quiescence. Make the six original
 * fault entries bind their selected metadata before another user handler can
 * run. Preserve flags (including restart), entry identity and logical masks.
 */
static int widen_host_fault_routes(void) {
  const int faults[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGSYS};
  struct sigaction before[GW_SIGNAL_FAULT_LANES];
  unsigned changed = 0;
  int error = 0;
  for (int i = 0; i < GW_SIGNAL_FAULT_LANES; ++i) {
    struct disposition *record =
        atomic_load_explicit(&current[faults[i]], memory_order_acquire);
    if (!record || !record->entry ||
        (!fault_routed(faults[i]) && (record->action.sa_handler == SIG_DFL ||
                                      record->action.sa_handler == SIG_IGN))) {
      continue;
    }
    if (system_action(faults[i], NULL, &before[i])) {
      error = errno;
      break;
    }
    if (before[i].sa_sigaction != record->entry) {
      if ((record->action.sa_flags & SA_RESETHAND) &&
          before[i].sa_handler == SIG_DFL) {
        continue;
      }
      error = EBUSY;
      break;
    }

    struct sigaction wide = before[i];
    wide_fault_mask(&wide);
    if (system_action(faults[i], &wide, NULL)) {
      error = errno;
      break;
    }
    changed |= 1u << i;
  }
  if (!error) {
    return 0;
  }

  for (int i = GW_SIGNAL_FAULT_LANES - 1; i >= 0; --i) {
    if ((changed & (1u << i)) && system_action(faults[i], &before[i], NULL)) {
      error = signal_router_quarantine();
    }
  }
  return error;
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
  uint64_t state =
      atomic_load_explicit(&ignore_state[number], memory_order_acquire);
  *ignored = state & 1;
  return state >> 1;
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

/* The two entry gateways already run under the kernel's full catchable mask
 * here. They can execute before Host TLS or the ARM64 platform register is
 * restored. Keep this lookup free of libc and thread-local storage. */
static inline __attribute__((always_inline)) struct disposition *find_prefix(
    struct disposition *records, uintptr_t pc, uintptr_t token) {
  for (struct disposition *record = records; record; record = record->next) {
    uintptr_t entry = (uintptr_t)record->entry;
    if (entry &&
        (token ? token == (uintptr_t)record : pc >= entry && pc - entry < 32)) {
      return record;
    }
  }
  return NULL;
}

static inline __attribute__((always_inline)) uintptr_t
frame_pc(const ucontext_t *frame) {
#if defined(__x86_64__)
  return frame->uc_mcontext.gregs[REG_RIP];
#else
  return frame->uc_mcontext.pc;
#endif
}

static inline __attribute__((always_inline)) int shared_prefix(uintptr_t pc) {
  return pc >= (uintptr_t)domain_signal_entry &&
         pc < (uintptr_t)domain_signal_masked;
}

static inline __attribute__((always_inline)) const ucontext_t *prefix_parent(
    const ucontext_t *frame, int shared, uintptr_t pc) {
  uintptr_t start = (uintptr_t)domain_signal_entry;
#if defined(__x86_64__)
  return (const void *)
      frame->uc_mcontext.gregs[shared && pc != start ? REG_R9 : REG_RDX];
#else
  return (const void *)frame->uc_mcontext.regs[shared && pc != start ? 15 : 2];
#endif
}

static inline __attribute__((always_inline)) uintptr_t
prefix_token(const ucontext_t *frame, uintptr_t pc) {
#if defined(__x86_64__)
  (void)pc;
  return frame->uc_mcontext.gregs[REG_RCX];
#else
  return frame->uc_mcontext
      .regs[pc < (uintptr_t)domain_signal_token_saved ? 3 : 17];
#endif
}

static inline __attribute__((always_inline)) uint64_t
mask_bits(const sigset_t *mask) {
  uint64_t bits;
  __builtin_memcpy(&bits, mask, sizeof bits);
  return bits;
}

static __attribute__((no_stack_protector)) int inspect_prefix(
    ucontext_t *interrupted, uint64_t *entry_mask) {
  if (!atomic_load_explicit(&routing_complete, memory_order_acquire)) {
    return 0;
  }

  const ucontext_t *frame = interrupted;
  uint64_t added = 0, requested = 0;
  int found = 0;
  for (;;) {
    uintptr_t pc = frame_pc(frame);
    int shared = shared_prefix(pc);
    if (!shared && (pc & 4095) >= 32) {
      /* Entry mappings start on a system page boundary, at least 4 KiB. */
      break;
    }
    uintptr_t token = shared ? prefix_token(frame, pc) : 0;
    while (atomic_flag_test_and_set_explicit(&writer, memory_order_acquire)) {
    }
    struct disposition *record = find_prefix(reclaim_batch, pc, token);
    if (!record) {
      record = find_prefix(allocations, pc, token);
    }
    int before_binding = shared || record;
    if (entry_mask && record) {
      /* The nested handler can save its interrupted ucontext. The owning
       * native continuation barrier decides when that saved PC is dead. */
      if (record->ordinary) {
        record->exposed_context = 1;
      }
      added |= record->prefix_mask;
      requested |= mask_bits(&record->action.sa_mask);
    }
    atomic_flag_clear_explicit(&writer, memory_order_release);

    if (!before_binding) {
      break;
    }
    found = 1;
    if (!entry_mask) {
      break;
    }
    /* Linux can stack several entries before executing the first one. The
     * interrupted prefix still has its original kernel ucontext argument. */
    frame = prefix_parent(frame, shared, pc);
  }
  if (added) {
    /* The older entries are now pinned. Strip their synthetic private bits
     * before either presenting or restoring the interrupted native frame. */
    uint64_t prior = mask_bits(&frame->uc_sigmask);
    if (gw_signal_wait_entry_mask) {
      prior = gw_signal_wait_entry_mask(frame, prior);
    }
    uint64_t synthetic = added & ~(requested | prior);
    uint64_t saved = mask_bits(&interrupted->uc_sigmask) & ~synthetic;
    __builtin_memcpy(&interrupted->uc_sigmask, &saved, sizeof saved);
    *entry_mask &= ~synthetic;
  }
  return found;
}

__attribute__((no_stack_protector, visibility("hidden"))) void
signal_router_expose_prefix(void *frame, uint64_t *entry_mask) {
  inspect_prefix(frame, entry_mask);
}

__attribute__((no_stack_protector, visibility("hidden"))) int
signal_router_probe_prefix(void *frame) {
  return inspect_prefix(frame, NULL);
}

__attribute__((no_stack_protector)) uint64_t
signal_router_entry_mask(void *token, uint64_t mask, uint64_t interrupted) {
  const struct disposition *record = token;
  uint64_t extra = record ? record->prefix_mask : 0;
  return (mask & ~extra) | (interrupted & extra);
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

static int initialize_locked(signal_router_backend backend) {
  if (signal_router_error()) {
    return EIO;
  }

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
                            signal_suspend_prepare, signal_fd_prepare,
                            signal_sleep_prepare};
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
                          SIGQUIT, 50,      51,      52,     53,      62};
  /* Linux JRE networking reserves kernel 62 (Host SIGRTMAX - 2), separately
   * from HotSpot's USR2 suspend/resume channel. Guest's dynamic SIGRTMAX must
   * not be used to calculate this Host-owned number.
   * https://github.com/openjdk/jdk17u/blob/master/src/java.base/linux/native/libnet/linux_close.c
   * https://github.com/openjdk/jdk17u/blob/master/src/java.base/unix/native/libnio/ch/NativeThread.c
   */
  host_reservations = 0;
  for (unsigned i = 0; i < sizeof reserved / sizeof reserved[0]; ++i) {
    host_reservations |= UINT64_C(1) << (reserved[i] - 1);
  }
#ifdef __ANDROID__
  for (int number = 32; number < SIGRTMIN; ++number) {
    host_reservations |= UINT64_C(1) << (number - 1);
  }
#elif defined(__GLIBC__)
  host_reservations |= UINT64_C(3) << (GW_GNU_HOST_CANCEL - 1);
#endif
  /* Routing a Host registration is not an exclusive VM reservation. Untagged
   * external packets belong to Host on every public signal, including USR1,
   * ALRM, CHLD and unreserved RT numbers. Bionic private handlers also need
   * Host TLS restoration; owning GNU Host NPTL entries remain native. */
  int complete = domain_signal_masked != NULL;
  memset(managed, 0, sizeof managed);
  for (int number = 1; number < NSIG; ++number) {
    if (number == SIGKILL || number == SIGSTOP ||
#ifndef __ANDROID__
        (number >= 32 && number < SIGRTMIN) ||
#endif
        (atomic_load(&guest_private) & (UINT64_C(1) << (number - 1)))) {
      continue;
    }
    if (system_action(number, NULL, &original[number])) {
      return errno;
    }
    atomic_store(&current[number], NULL);
    atomic_store(&ignore_state[number], original[number].sa_handler == SIG_IGN);
    managed[number] = 1;
    if (original[number].sa_handler != SIG_DFL &&
        original[number].sa_handler != SIG_IGN) {
      complete = 0;
    }
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
  atomic_store_explicit(&routing_complete, complete, memory_order_release);
  atomic_store_explicit(&enabled, 1, memory_order_release);
  return 0;
}

int signal_router_init_with_backend(signal_router_backend backend) {
  uint64_t mask;
  lock_writer(&mask);
  int error = initialize_locked(backend);
  unlock_writer(mask);
  return error;
}

int signal_router_init(void) {
  return signal_router_init_with_backend(dlsym(RTLD_NEXT, "sigaction"));
}

int signal_router_lease_guest_nptl(void) {
  if (!signal_router_enabled() || signal_router_error()) {
    return signal_router_error() ? EIO : EINVAL;
  }

  uint64_t mask;
  lock_writer(&mask);
  if (atomic_load_explicit(&guest_private, memory_order_acquire)) {
    unlock_writer(mask);
    return 0;
  }
  if (reclaim_batch) {
    unlock_writer(mask);
    return EBUSY;
  }

  const int numbers[] = {63, 64};
  int error = 0;
  for (unsigned i = 0; i < sizeof numbers / sizeof numbers[0]; ++i) {
    int number = numbers[i];
    struct sigaction actual;
    if (!managed[number] || atomic_load(&current[number]) ||
        (signal_router_reserved() & (UINT64_C(1) << (number - 1)))) {
      error = EBUSY;
      break;
    }
    if (system_action(number, NULL, &actual)) {
      error = errno;
      break;
    }
    if (original[number].sa_handler != SIG_DFL ||
        actual.sa_handler != SIG_DFL) {
      error = EBUSY;
      break;
    }
  }
  if (!error) {
    for (unsigned i = 0; i < sizeof numbers / sizeof numbers[0]; ++i) {
      managed[numbers[i]] = 0;
    }
    atomic_store_explicit(&guest_private, GW_GUEST_NPTL_SIGNALS,
                          memory_order_release);
  }
  unlock_writer(mask);
  return error;
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

int signal_router_reclaim_signal(void) {
  return atomic_load_explicit(&reclaim_signal, memory_order_acquire);
}

static struct sigaction reclaim_action(void) {
  struct sigaction action = {.sa_sigaction = gw_signal_quiescence_ack,
                             .sa_flags = SA_SIGINFO | SA_RESTART};
  /* Use the interrupted stack. A bridged alternate stack can still contain
   * a parked activation even when its interrupted SP is on another stack. */
  wide_fault_mask(&action);
  return action;
}

static int verify_reclaim_action(int number) {
  struct sigaction actual;
  if (system_action(number, NULL, &actual)) {
    return errno;
  }
  struct sigaction expected = reclaim_action();
  const unsigned relevant =
      SA_SIGINFO | SA_RESTART | SA_ONSTACK | SA_NODEFER | SA_RESETHAND;
  return actual.sa_sigaction == expected.sa_sigaction &&
                 !((actual.sa_flags ^ expected.sa_flags) & relevant) &&
                 kernel_mask_is_full(&actual)
             ? 0
             : EBUSY;
}

int gw_host_signal_reclaim_enable(int number) {
  if (signal_router_error()) {
    return EIO;
  }
  if (!signal_router_enabled() || number < SIGRTMIN || number > SIGRTMAX) {
    return EINVAL;
  }

  uint64_t previous;
  lock_writer(&previous);
  int error;
  if (signal_router_reclaim_signal()) {
    error = EALREADY;
  } else if (!managed[number] || atomic_load(&current[number]) ||
             ((signal_router_reserved() | GW_GUEST_NPTL_SIGNALS) &
              (UINT64_C(1) << (number - 1))) ||
             original[number].sa_handler != SIG_DFL) {
    error = EBUSY;
  } else {
    struct sigaction existing;
    if (system_action(number, NULL, &existing)) {
      error = errno;
    } else if (!same_action(&existing, &original[number])) {
      error = EBUSY;
    } else {
      struct sigaction action = reclaim_action();
      if (system_action(number, &action, NULL)) {
        error = errno;
      } else {
        reclaim_process = getpid();
        atomic_store_explicit(&reclaim_signal, number, memory_order_release);
        error = verify_reclaim_action(number);
        if (error) {
          error = signal_router_quarantine();
        }
      }
    }
  }
  unlock_writer(previous);
  return error;
}

/* Adopt inherited native actions during serialized startup. Immutable
 * entries are complete before installation, including during delivery. */
int signal_router_adopt(void) {
  if (signal_router_error()) {
    return EIO;
  }

  if (!atomic_load_explicit(&enabled, memory_order_acquire)) {
    return EINVAL;
  }
  for (int number = 1; number < NSIG; ++number) {
    if (!managed[number] || number == signal_router_reclaim_signal()) {
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
         strstr(owner.dli_fname, "libart.so") ||
         strstr(owner.dli_fname, "libartd.so"))) {
      /* ART reads pthread TLS before its special handlers. Wrapping only the
       * user handler is too late; do not mutate this chain speculatively. */
      return ENOTSUP;
    }
  }
  for (int number = 1; number < NSIG; ++number) {
    if (managed[number] && number != signal_router_reclaim_signal() &&
        original[number].sa_handler != SIG_DFL &&
        original[number].sa_handler != SIG_IGN &&
        signal_router_action(number, &original[number], NULL)) {
      /* The transaction owner restores all layers exactly once. Partial
       * adoption retains immutable entries until process disposal. */
      return errno;
    }
  }
  atomic_store_explicit(&routing_complete, domain_signal_masked != NULL,
                        memory_order_release);
  return 0;
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

#ifdef __GLIBC__
static int setxid_action(const struct kernel_action *action,
                         struct kernel_action *previous) {
  return syscall(SYS_rt_sigaction, GW_GNU_HOST_SETXID, action, previous,
                 sizeof(uint64_t))
             ? errno
             : 0;
}

static int verify_setxid(void) {
  if (!atomic_load_explicit(&host_setxid_active, memory_order_acquire)) {
    return 0;
  }
  struct kernel_action actual = {0};
  int error = setxid_action(NULL, &actual);
  return error                                                 ? error
         : memcmp(&actual, &routed_setxid, sizeof actual) != 0 ? EBUSY
                                                               : 0;
}

static int install_setxid(void) {
  struct kernel_action action = {0};
  int error = setxid_action(NULL, &action);
  if (error) {
    return error;
  }

  /* GNU initializes setxid before application execution and only sends it
   * to its own live pthread descriptors. The handler must update the same
   * descriptor and acknowledge the owning libc's broadcast futex.
   * https://github.com/bminor/glibc/blob/glibc-2.42/nptl/nptl_setxid.c */
  Dl_info owner, expected;
  void *libc = dlsym(RTLD_NEXT, "pthread_self");
  if (action.handler <= 1 ||
      (action.flags & (SA_SIGINFO | SA_RESTART)) != (SA_SIGINFO | SA_RESTART) ||
      !libc || !dladdr(libc, &expected) ||
      !dladdr((void *)action.handler, &owner) ||
      owner.dli_fbase != expected.dli_fbase) {
    return ENOTSUP;
  }

  struct disposition *record =
      mmap(NULL, sizeof *record, PROT_READ | PROT_WRITE,
           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (record == MAP_FAILED) {
    return errno;
  }
  record->internal = 1;
  record->action.sa_sigaction =
      (void (*)(int, siginfo_t *, void *))action.handler;
  if (make_entry(record)) {
    error = errno;
    munmap(record, sizeof *record);
    return error;
  }

  native_setxid = action;
  routed_setxid = action;
  routed_setxid.handler = (uintptr_t)record->entry;
  error = setxid_action(&routed_setxid, NULL);
  if (error) {
    munmap(record->entry, record->entry_size);
    munmap(record, sizeof *record);
    return error;
  }

  record->next = allocations;
  allocations = record;
  atomic_fetch_add_explicit(&retained, 1, memory_order_relaxed);
  atomic_store_explicit(&current[GW_GNU_HOST_SETXID], record,
                        memory_order_release);
  atomic_store_explicit(&host_setxid_active, 1, memory_order_release);
  return 0;
}
#endif

int signal_router_route_host_setxid(void) {
#ifdef __GLIBC__
  if (!signal_router_enabled() || signal_router_error()) {
    return EINVAL;
  }
  uint64_t mask;
  lock_writer(&mask);
  int error = atomic_load_explicit(&host_setxid_active, memory_order_acquire)
                  ? verify_setxid()
                  : install_setxid();
  unlock_writer(mask);
  return error;
#else
  return 0; /* Other Host libcs use their own registration integration. */
#endif
}

/* Exported by the Host executable so libraries' public sigaction calls
 * enter here. Queries return the logical disposition, not the routing entry.
 * Explicit dlsym(libc, ...) and raw syscalls are outside this interception. */
static int router_action(int number, const struct sigaction *action,
                         struct sigaction *previous, int internal) {
  if (signal_router_error()) {
    errno = EIO;
    return -1;
  }

  if (!system_action) {
    /* Bionic's own constructors can call an executable-interposed sigaction
     * before main, including fdtrack's private signal registration. Forward
     * those calls natively instead of silently losing the initial handler.
     * https://android.googlesource.com/platform/bionic/+/android13-release/libc/bionic/fdtrack.cpp
     */
    int saved = errno;
    signal_router_backend backend = dlsym(RTLD_NEXT, "sigaction");
    if (!backend || backend == signal_router_action) {
      errno = ENOSYS;
      return -1;
    }
    signal_router_backend empty = NULL;
    atomic_compare_exchange_strong_explicit(&system_action, &empty, backend,
                                            memory_order_acq_rel,
                                            memory_order_acquire);
    errno = saved;
  }
  if (number == signal_router_reclaim_signal() && number > 0) {
    errno = EBUSY;
    return -1;
  }
  if (number > 0 && number < NSIG &&
      (atomic_load_explicit(&guest_private, memory_order_acquire) &
       (UINT64_C(1) << (number - 1)))) {
    errno = EBUSY;
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
  if (number == signal_router_reclaim_signal() || (old && old->internal) ||
      (internal && (old || original[number].sa_handler != SIG_DFL))) {
    unlock_writer(mask);
    errno = EBUSY;
    return -1;
  }
  if (internal) {
    struct sigaction actual;
    int result = system_action(number, NULL, &actual);
    if (result || !same_action(&actual, &original[number])) {
      int error = result ? errno : EBUSY;
      unlock_writer(mask);
      errno = error;
      return -1;
    }
  }
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
  next->internal = internal;
  next->ignore_generation = old ? old->ignore_generation : 0;
  if (action->sa_handler == SIG_IGN) {
    if (next->ignore_generation == (UINT64_MAX >> 1)) {
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
  if (lane >= 0 && next->entry && installed.sa_sigaction == next->entry) {
    wide_fault_mask(&installed);
  }

  if (next->entry && !internal && lane < 0 &&
      atomic_load_explicit(&routing_complete, memory_order_acquire) &&
      !kernel_mask_is_full(&installed)) {
    /* Any nested public handler comes through the entry inspection. Native
     * libc-private handlers have their own gateways and must wait until the
     * entry snapshot. This changes only private bits while binding the entry.
     */
    next->prefix_mask = native_prefix_mask();
    uint64_t bits;
    memcpy(&bits, &installed.sa_mask, sizeof bits);
    bits |= next->prefix_mask;
    memcpy(&installed.sa_mask, &bits, sizeof bits);
    next->ordinary = 1;
  }

  struct sigaction replaced;
  int status = system_action(number, &installed, &replaced);
  int error = errno;
  int exposed = !status && next->entry && installed.sa_sigaction == next->entry;
  int guarded = !exposed || kernel_mask_is_full(&installed);
  if (!status && lane >= 0) {
    struct sigaction helper = notification_action(next);
    if (system_action(notifications[lane], &helper, NULL)) {
      error = errno;
      if (system_action(number, &replaced, NULL)) {
        atomic_store_explicit(&damaged, 1, memory_order_release);
        error = EIO;
      }
      status = -1;
    }
  }
  if (!status && next->entry && guarded && !internal) {
    /* The approved bottom backend must actually publish the outer masks.
     * SA_RESETHAND can consume the PC during this query, leaving its mask. */
    struct sigaction actual;
    next->guarded =
        (!exposed || (!system_action(number, NULL, &actual) &&
                      kernel_mask_is_full(&actual))) &&
        (lane < 0 || (!system_action(notifications[lane], NULL, &actual) &&
                      kernel_mask_is_full(&actual)));
  }
  if (!status && next->ordinary && next->prefix_mask) {
    struct sigaction actual = {0};
    uint64_t mask;
    int read = system_action(number, NULL, &actual);
    memcpy(&mask, &actual.sa_mask, sizeof mask);
    next->ordinary = !read && (mask & next->prefix_mask) == next->prefix_mask;
  }
  if (!status) {
    next->next = allocations;
    allocations = next;
    atomic_fetch_add_explicit(&retained, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&records, 1, memory_order_relaxed);
    atomic_store_explicit(&current[number], next, memory_order_release);
    uint64_t ignored = action->sa_handler == SIG_IGN;
    atomic_store_explicit(&ignore_state[number],
                          (next->ignore_generation << 1) | ignored,
                          memory_order_release);
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

int signal_router_action(int number, const struct sigaction *action,
                         struct sigaction *previous) {
  return router_action(number, action, previous, 0);
}

int signal_router_internal_action(int number,
                                  void (*entry)(int, siginfo_t *, void *)) {
  if (!signal_router_enabled() || !entry || number < SIGRTMIN ||
      number > SIGRTMAX ||
      (host_reservations & (UINT64_C(1) << (number - 1)))) {
    return EINVAL;
  }
  struct sigaction action = {.sa_sigaction = entry,
                             .sa_flags = SA_SIGINFO | SA_RESTART};
  /* This helper belongs to the runtime and runs before application selection.
   * Keep its kernel frame on the interrupted stack. An alternate stack may
   * still contain a suspended Host frame while a nested Host handler executes
   * on its ordinary stack. Asking the kernel for SA_ONSTACK here would reuse
   * that alternate stack from its top.
   * https://github.com/torvalds/linux/blob/v6.12/kernel/signal.c
   * https://github.com/torvalds/linux/blob/v6.12/arch/x86/kernel/signal.c */
  wide_fault_mask(&action);
  return router_action(number, &action, NULL, 1) ? errno : 0;
}

int signal_router_dispatch_internal(int number, siginfo_t *information,
                                    void *context, void *token) {
  struct disposition *record = token;
  if (!record || !record->internal) {
    return 0;
  }
  int saved_errno = errno;
  void (*entry)(int, siginfo_t *, void *) = record->action.sa_sigaction;
  entry(number, information, context);
  errno = saved_errno;
  return 1;
}

int signal_router_guest_faults(void) {
  if (signal_router_error()) {
    return EIO;
  }

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
      int error = errno;
      atomic_store_explicit(&damaged, 1, memory_order_release);
      return error;
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
                            const struct sigaction *action) {
  int saved_errno = errno;
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
    struct sigaction action = ((struct disposition *)token)->action;
    invoke_delivery(number, info, context, &action);
  }
}

struct host_delivery {
  int number;
  siginfo_t *information;
  void *context;
  struct sigaction action;
  uint64_t mask;
  const stack_t *stack;
};

static void host_delivery_run(void *argument) {
  struct host_delivery *delivery = argument;
  if (delivery->stack) {
    gw_signal_stack_set(delivery->stack);
  }
  extern void gw_signal_wait_host_delivery(int) __attribute__((weak));
  if (gw_signal_wait_host_delivery) {
    gw_signal_wait_host_delivery(!!(delivery->action.sa_flags & SA_RESTART));
  }
  gw_nonlocal_mask_restore(signal_host_physical_mask
                               ? signal_host_physical_mask(delivery->mask)
                               : delivery->mask);
  extern void signal_router_delivery_observe(int) __attribute__((weak));
  if (signal_router_delivery_observe) {
    signal_router_delivery_observe(delivery->number);
  }
  invoke_delivery(delivery->number, delivery->information, delivery->context,
                  &delivery->action);
  gw_nonlocal_mask_block();
  extern void gw_signal_wait_host_return(void) __attribute__((weak));
  if (gw_signal_wait_host_return) {
    gw_signal_wait_host_return();
  }
}

void signal_router_forward_domain(int number, siginfo_t *info, void *context,
                                  void *token, uint64_t host_mask,
                                  const stack_t *host_stack, uintptr_t host_sp,
                                  int foreign) {
  signal_router_forward_selected_domain(number, info, context, token, host_mask,
                                        host_stack, host_sp, foreign, -1);
}

static void forward_selected(int number, siginfo_t *info, void *context,
                             void *token, uint64_t host_mask,
                             const stack_t *host_stack, uintptr_t host_sp,
                             int foreign, int selected,
                             const struct gw_host_stack_floors *floors,
                             uintptr_t *guest_floor) {
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
      top = floors && floors->alternate
                ? gw_signal_stack_top(floors->alternate)
                : (uintptr_t)host_stack->ss_sp + host_stack->ss_size;
      if ((unsigned)host_stack->ss_flags & GW_SS_AUTODISARM) {
        effective = (stack_t){.ss_flags = SS_DISABLE};
      }
    } else {
      /* A private entry may be on the original ordinary stack, or below a
       * Host handler already on its alternate stack. Preserve all of those
       * suspended frames rather than starting again below the original PC. */
      uintptr_t floor = 0;
      if (floors) {
        floor = gw_stack_contains(host_stack, host_sp) ? floors->alternate
                                                       : floors->ordinary;
      }
      top = gw_signal_stack_top(floor && floor < host_sp ? floor : host_sp);
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
  /* No saved callback frame may borrow the registration. Take its snapshot
   * before host_delivery_run permits a rendezvous or an escaping handler. */
  struct host_delivery delivery = {.number = number,
                                   .information = info,
                                   .context = context,
                                   .action = record->action,
                                   .mask = mask,
                                   .stack = configure ? &effective : NULL};
  domain_stack_call(host_delivery_run, &delivery, top, guest_floor);
  if (configure) {
    gw_signal_stack_set(&physical);
  }
  gw_nonlocal_mask_restore(original_mask);
}

void signal_router_forward_selected_domain(
    int number, siginfo_t *info, void *context, void *token, uint64_t host_mask,
    const stack_t *host_stack, uintptr_t host_sp, int foreign, int selected) {
  forward_selected(number, info, context, token, host_mask, host_stack, host_sp,
                   foreign, selected, NULL, NULL);
}

void signal_router_forward_selected_suspended(
    int number, siginfo_t *info, void *context, void *token, uint64_t host_mask,
    const stack_t *host_stack, uintptr_t host_sp,
    const struct gw_host_stack_floors *floors, int selected,
    uintptr_t *guest_floor) {
  forward_selected(number, info, context, token, host_mask, host_stack, host_sp,
                   1, selected, floors, guest_floor);
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

static int verify_locked(void) {
  if (signal_router_error()) {
    return EIO;
  }

  if (signal_stdio_verify) {
    int error = signal_stdio_verify();
    if (error) {
      return error;
    }
  }
#ifdef __GLIBC__
  int private_error = verify_setxid();
  if (private_error) {
    return private_error;
  }
#endif

  int proof = signal_router_reclaim_signal();
  if (proof) {
    int error = verify_reclaim_action(proof);
    if (error) {
      return error;
    }
  }
  const int *notifications = signal_router_fault_notifications();
  if (notifications) {
    for (int i = 0; i < GW_SIGNAL_FAULT_LANES; ++i) {
      struct sigaction action;
      if (system_action(notifications[i], NULL, &action)) {
        return errno;
      }
      const int faults[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGSYS};
      struct disposition *record = current_token(faults[i]);
      if (action.sa_sigaction != record->entry ||
          !(action.sa_flags & SA_SIGINFO) || (action.sa_flags & SA_RESETHAND) ||
          ((action.sa_flags ^ record->action.sa_flags) & SA_RESTART) ||
          !kernel_mask_is_full(&action)) {
        return EBUSY;
      }
    }
  }

  for (int number = 1; number < NSIG; ++number) {
    struct disposition *record =
        atomic_load_explicit(&current[number], memory_order_acquire);
    if (!managed[number] || number == proof) {
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
      int wide = notifications && fault_lane(number) >= 0 && custom;
      if (wide && !kernel_mask_is_full(&actual)) {
        return EBUSY;
      }
      if (!wide) {
        for (int blocked = 1; blocked < NSIG; ++blocked) {
          if (blocked != SIGKILL && blocked != SIGSTOP &&
              sigismember(&actual.sa_mask, blocked) !=
                  ((record->prefix_mask & (UINT64_C(1) << (blocked - 1)))
                       ? 1
                       : sigismember(&record->action.sa_mask, blocked))) {
            return EBUSY;
          }
        }
      }
    }
  }
  return 0;
}

int signal_router_verify(void) {
  uint64_t mask;
  lock_writer(&mask);
  int error = verify_locked();
  unlock_writer(mask);
  return error;
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

struct restore_step {
  int number, written;
  struct sigaction before;
  const struct sigaction *target;
};

static int snapshot_restore(struct restore_step *steps, unsigned *count,
                            uint64_t *notification_bits) {
  const int *notifications = signal_router_fault_notifications();
  int proof = signal_router_reclaim_signal();
  for (int number = 1; number < NSIG; ++number) {
    if (number == proof) {
      continue;
    }
    const struct sigaction *target = managed[number] ? &original[number] : NULL;
    if (notifications) {
      for (int i = 0; i < GW_SIGNAL_FAULT_LANES; ++i) {
        if (number == notifications[i]) {
          target = &notification_original[i];
          *notification_bits |= UINT64_C(1) << (number - 1);
        }
      }
    }
    if (!target) {
      continue;
    }

    struct restore_step *step = &steps[*count];
    step->number = number;
    step->written = 0;
    step->target = target;
    if (system_action(number, NULL, &step->before)) {
      return errno;
    }
    ++*count;
  }
  return 0;
}

static int apply_restore(struct restore_step *steps, unsigned count,
                         unsigned *changed) {
  for (; *changed < count; ++*changed) {
    struct restore_step *step = &steps[*changed];
    if (same_action(&step->before, step->target)) {
      continue;
    }
    if (system_action(step->number, step->target, NULL)) {
      return errno;
    }
    step->written = 1;
  }
  return 0;
}

static int rollback_restore(struct restore_step *steps, unsigned changed) {
  int error = 0;
  while (changed) {
    struct restore_step *step = &steps[--changed];
    if (step->written && system_action(step->number, &step->before, NULL)) {
      error = EIO;
    }
  }
  return error;
}

int signal_router_restore(void) {
  if (signal_router_error()) {
    return EIO;
  }

  uint64_t previous;
  lock_writer(&previous);
  /* A dispatcher owns the router from before pthread_create until after
   * join. Serialize that admission with restoration, including both windows
   * in which the dispatcher cannot yet accept requests. */
  if (reclaim_batch ||
      (signal_host_relay_active && signal_host_relay_active())) {
    unlock_writer(previous);
    return EBUSY;
  }

  /* The caller has joined recipients, destroyed the VM and frozen
   * descriptor owners. Read the channel and its owner under the writer. */
  const int *notifications = signal_router_fault_notifications();
  if (notifications &&
      (notification_owner != syscall(SYS_gettid) ||
       (signal_deferred_recipients && signal_deferred_recipients()))) {
    unlock_writer(previous);
    return EBUSY;
  }
  int error = signal_fd_channel_busy ? signal_fd_channel_busy() : 0;
  if (!error) {
    error = verify_locked();
  }
  struct restore_step steps[NSIG];
  unsigned count = 0, changed = 0;
  uint64_t notification_bits = 0;
  if (!error) {
    error = snapshot_restore(steps, &count, &notification_bits);
  }
  if (!error) {
    error = apply_restore(steps, count, &changed);
  }
#ifdef __GLIBC__
  int private_written = 0;
  if (!error && atomic_load(&host_setxid_active)) {
    error = setxid_action(&native_setxid, NULL);
    private_written = !error;
  }
#endif
  if (!error && notifications && signal_stdio_disable) {
    error = signal_stdio_disable();
    if (error == EIO) {
      atomic_store_explicit(&damaged, 1, memory_order_release);
    }
  }

  if (error) {
    int rollback = rollback_restore(steps, changed);
#ifdef __GLIBC__
    if (private_written && setxid_action(&routed_setxid, NULL)) {
      rollback = EIO;
    }
#endif
    if (rollback) {
      atomic_store_explicit(&damaged, 1, memory_order_release);
      error = EIO;
    }
    unlock_writer(previous);
    return error;
  }

  atomic_store_explicit(&deferred_faults, 0, memory_order_release);
  atomic_store_explicit(&routing_complete, 0, memory_order_release);
  for (struct disposition *record = allocations; record;
       record = record->next) {
    record->ordinary = 0;
  }
  atomic_store(&enabled, 0);
  atomic_store(&guest_faults, 0);
#ifdef __GLIBC__
  atomic_store_explicit(&host_setxid_active, 0, memory_order_release);
  atomic_store_explicit(&current[GW_GNU_HOST_SETXID], NULL,
                        memory_order_release);
#endif

  /* Caller has joined recipients, destroyed the VM and frozen descriptor
   * owners. Preserve all non-private mask changes across runtime shutdown. */
  if (notifications) {
    previous = (previous & ~notification_bits) | notification_initial_mask;
  }
  unlock_writer(previous);
  return 0;
}

unsigned signal_router_registrations(void) {
  return atomic_load(&records);
}

uint64_t signal_router_reserved(void) {
  uint64_t mask = host_reservations;
  int proof = signal_router_reclaim_signal();
  if (proof) {
    mask |= UINT64_C(1) << (proof - 1);
  }
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

static struct disposition *detach_retired(int online) {
  int live = atomic_load_explicit(&enabled, memory_order_acquire);
  uint64_t native = native_prefix_mask();
  struct disposition *batch = NULL;
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
    int ordinary = record->ordinary && !record->exposed_context &&
                   (record->prefix_mask & native) == native;
    if (published ||
        (online && record->entry && !record->guarded && !ordinary)) {
      link = &record->next;
      continue;
    }
    *link = record->next;
    record->next = batch;
    batch = record;
  }
  if (!live && !online) {
    for (int number = 1; number < NSIG; ++number) {
      atomic_store_explicit(&current[number], NULL, memory_order_relaxed);
    }
  }
  return batch;
}

static unsigned release_retired(struct disposition *batch) {
  unsigned count = 0;
  while (batch) {
    struct disposition *record = batch;
    batch = record->next;
    if (record->entry) {
      if (syscall(SYS_munmap, record->entry, record->entry_size)) {
        record->next = allocations;
        allocations = record;
        continue;
      }
      record->entry = NULL;
    }
    if (syscall(SYS_munmap, record, sizeof *record)) {
      record->next = allocations;
      allocations = record;
      continue;
    }
    ++count;
  }
  atomic_fetch_sub_explicit(&retained, count, memory_order_relaxed);
  return count;
}

int signal_router_reclaim(const struct signal_router_quiescence *gate,
                          unsigned *reclaimed) {
  if (signal_router_error()) {
    return EIO;
  }
  if (!gate || gate->version != GW_SIGNAL_ROUTER_QUIESCENCE_V1 ||
      gate->size != sizeof *gate || !gate->enter || !gate->leave ||
      !reclaimed) {
    return EINVAL;
  }

  uint64_t mask;
  lock_writer(&mask);
  if (atomic_flag_test_and_set_explicit(&reclaimer, memory_order_acquire)) {
    unlock_writer(mask);
    return EBUSY;
  }
  int error = gate->enter(gate->context);
  if (!error) {
    if (signal_host_relay_busy && signal_host_relay_busy()) {
      error = EBUSY;
    } else {
      *reclaimed = release_retired(detach_retired(0));
    }
    gate->leave(gate->context);
  }
  atomic_flag_clear_explicit(&reclaimer, memory_order_release);
  unlock_writer(mask);
  return error;
}

int gw_host_signal_reclaim(unsigned timeout_ms, unsigned *reclaimed) {
  if (!timeout_ms || !reclaimed) {
    return EINVAL;
  }
  int proof = signal_router_reclaim_signal();
  if (!proof || getpid() != reclaim_process) {
    return ENOTSUP;
  }

  uint64_t previous;
  lock_writer(&previous);
  if (atomic_flag_test_and_set_explicit(&reclaimer, memory_order_acquire)) {
    unlock_writer(previous);
    return EBUSY;
  }
  int error = signal_router_error();
  if (!error) {
    error = signal_router_enabled() ? verify_locked() : ENOTSUP;
  }
  struct disposition *batch = error ? NULL : detach_retired(1);
  reclaim_batch = batch;
  /* Freeze this batch, then let pending deliveries select their actions and
   * leave the masked prefix. They can need the writer to reach that point. */
  unlock_writer(UINT64_MAX);
  if (batch) {
    error = gw_signal_quiescence_check(proof, timeout_ms);
  }

  uint64_t masked;
  lock_writer(&masked);
  reclaim_batch = NULL;
  if (!error) {
    error = signal_router_error();
  }
  if (!error && batch && signal_host_relay_busy && signal_host_relay_busy()) {
    error = EBUSY;
  }
  struct disposition **link = &batch;
  while (*link) {
    struct disposition *record = *link;
    if (error || record->exposed_context) {
      *link = record->next;
      record->next = allocations;
      allocations = record;
    } else {
      link = &record->next;
    }
  }
  if (!error) {
    *reclaimed = release_retired(batch);
  }
  atomic_flag_clear_explicit(&reclaimer, memory_order_release);
  unlock_writer(previous);
  return error;
}
