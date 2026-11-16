#define _GNU_SOURCE
#include "signals.h"
#include "signal-stack.h"
#include "signal-consume.h"
#include "signal-relay.h"
#include "raw_syscall.h"
#include "nonlocal-mask.h"
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <string.h>
#if defined(__aarch64__)
#include <asm/sigcontext.h>
#endif

/* Snapshot only state not restored by the owning libc's native longjmp. The
 * original kernel frame is authoritative; a last cooperative swap can predate
 * a module's fenv changes. Host signal execution must not overwrite it.
 * https://github.com/torvalds/linux/blob/v6.12/arch/arm64/include/uapi/asm/sigcontext.h
 * https://github.com/torvalds/linux/blob/v6.12/arch/x86/include/uapi/asm/sigcontext.h
 */
static void save_guest_control(struct exchange *exchange, ucontext_t *context) {
  _Static_assert(sizeof context->uc_sigmask >= sizeof(uint64_t),
                 "kernel signal mask representation");
  memcpy(&exchange->interrupted_guest_mask, &context->uc_sigmask,
         sizeof exchange->interrupted_guest_mask);
  gw_signal_stack_get(&exchange->interrupted_guest_altstack);
#if defined(__x86_64__)
  if (!context->uc_mcontext.fpregs) {
    _exit(96);
  }
  exchange->interrupted_control[0] = context->uc_mcontext.fpregs->mxcsr;
  exchange->interrupted_control[1] = context->uc_mcontext.fpregs->cwd;
#else
  exchange->interrupted_control[2] = context->uc_mcontext.regs[18];
  unsigned char *records = context->uc_mcontext.__reserved;
  size_t offset = 0, size = sizeof context->uc_mcontext.__reserved;
  while (size - offset >= sizeof(struct _aarch64_ctx)) {
    struct _aarch64_ctx *head = (void *)(records + offset);
    if (head->size < sizeof *head || head->size > size - offset) {
      break;
    }
    if (head->magic == FPSIMD_MAGIC &&
        head->size >= sizeof(struct fpsimd_context)) {
      struct fpsimd_context *fp = (void *)head;
      exchange->interrupted_control[0] = fp->fpcr;
      exchange->interrupted_control[1] = fp->fpsr;
      return;
    }
    if (head->size % 16) {
      break;
    }
    offset += head->size;
  }
  _exit(96);
#endif
}

struct signal_slot signal_slots[DOMAIN_FIXTURE_CARRIERS];
/* Only access after Host TP restoration. Assembly still uses the stable
 * TLS-free registry, while Host mask/wait APIs avoid scanning that registry. */
static _Thread_local struct signal_slot *host_recipient;
static _Atomic unsigned deferred_recipients;
/* Standalone adoption fixtures have no consuming interposition target. */
extern int gw_signal_consume_busy(void) __attribute__((weak));
extern struct gw_signal_consumption *gw_signal_consume_interrupt(int)
    __attribute__((weak));
extern void gw_signal_consume_resume(struct gw_signal_consumption *)
    __attribute__((weak));

unsigned signal_deferred_recipients(void) {
  return atomic_load_explicit(&deferred_recipients, memory_order_acquire);
}

static int signal_try_register(struct signal_slot *, struct exchange *);

static void publish_recipient(struct signal_slot *slot) {
#ifdef GW_DOMAIN_CHAIN
  uint64_t mask = gw_nonlocal_mask_block();
  host_recipient = slot;
  /* Registration's mask wrapper ran before TLS named this recipient and kept
   * private lanes blocked. Publish first, then apply this recipient's logical
   * mask; otherwise its first notification can remain blocked indefinitely. */
  mask = signal_host_physical_mask(signal_host_logical_mask(mask));
  gw_nonlocal_mask_restore(mask);
#else
  host_recipient = slot;
#endif
}

#include "signal-registry.inc"
extern void domain_signal_entry(int, siginfo_t *, void *);
#ifdef GW_DOMAIN_CHAIN
#include "signal-router.h"
#include "nonlocal-mask.h"
_Atomic int domain_forwarded_faults;
_Atomic int domain_forwarded_async;
_Atomic int domain_guest_host_signals;
_Atomic int domain_expected_host_signals;
_Atomic int domain_sr_deliveries;
_Atomic int domain_sr_guest_deliveries;
static _Atomic long guest_tids[DOMAIN_FIXTURE_CARRIERS + 1];
static _Atomic uintptr_t guest_tps[DOMAIN_FIXTURE_CARRIERS + 1];
static _Atomic(uintptr_t) guest_fault_entry;

/* The pinned GNU service installs this before admitting module constructors
 * or pthreads. Called with Guest TP; no Host libc, TLS or errno access. */
__attribute__((no_stack_protector)) int signal_guest_fault_bind(
    int (*entry)(int, void *, void *)) {
  uintptr_t expected = 0;
  return entry && atomic_compare_exchange_strong_explicit(
                      &guest_fault_entry, &expected, (uintptr_t)entry,
                      memory_order_release, memory_order_relaxed)
             ? 0
             : EINVAL;
}

/* Genuine GNU pthreads keep their real TLS. Hardware faults stay in Guest;
 * asynchronous Host handlers explicitly run on the real Host dispatcher. */
__attribute__((no_stack_protector)) void domain_guest_thread_dispatch(
    int number, siginfo_t *info, void *context, void *token, uint64_t mask) {
  uintptr_t entry =
      atomic_load_explicit(&guest_fault_entry, memory_order_acquire);
  if (info->si_code > 0 &&
      (number == SIGSEGV || number == SIGBUS || number == SIGILL ||
       number == SIGFPE || number == SIGTRAP || number == SIGSYS)) {
    if (entry &&
        !((int (*)(int, void *, void *))entry)(number, info, context)) {
      return;
    }
  } else if (!signal_host_relay(number, info, context, token, mask)) {
    return;
  }
  gw_raw3(SYS_exit_group, 96, 0, 0);
  __builtin_unreachable();
}

/* Callable with Guest TLS: no libc, allocation, locks or stack protector. */
__attribute__((no_stack_protector)) void signal_note_guest_thread(
    int index, long tid, uintptr_t tp) {
  atomic_store_explicit(&guest_tps[index], tp, memory_order_relaxed);
  atomic_store_explicit(&guest_tids[index], tid, memory_order_release);
}

static void forward_host(int number, siginfo_t *info, void *context,
                         void *token, struct signal_slot *slot,
                         uintptr_t interrupted_tp, uint64_t entry_mask,
                         int selected) {
  if (number == SIGUSR2) {
    atomic_fetch_add_explicit(&domain_sr_deliveries, 1, memory_order_relaxed);
  }
  if (number == SIGSEGV) {
    atomic_fetch_add_explicit(&domain_forwarded_faults, 1,
                              memory_order_relaxed);
  } else {
    atomic_fetch_add_explicit(&domain_forwarded_async, 1, memory_order_relaxed);
  }
  if (slot && interrupted_tp != slot->host_tp &&
      slot->exchange->host_mask_valid) {
    signal_router_forward_selected_domain(
        number, info, context, token, slot->exchange->host_execution_mask,
        &slot->exchange->host_altstack, slot->exchange->host.sp, 1, selected);
  } else {
    ucontext_t *frame = context;
    uint64_t mask;
    memcpy(&mask, &frame->uc_sigmask, sizeof mask);
    if (signal_router_fault_notifications()) {
      /* A native Host handler may inspect or edit this saved mask. Private
       * notification bits are an implementation detail even for unrelated
       * Host signals; expose the logical mask and translate edits on return.
       * Nonlocal exits consume the logical snapshot in their owning libc. */
      mask = signal_host_logical_mask(mask);
      memcpy(&frame->uc_sigmask, &mask, sizeof mask);
    }
    /* A mask-wait's ucontext saves the mask to restore after the wait, not
     * necessarily the temporary mask under which this handler must execute.
     * Assembly captured the actual kernel handler-entry mask before blocking.
     * https://github.com/torvalds/linux/blob/v6.12/include/linux/sched/signal.h
     * See sigmask_to_save(), including current->saved_sigmask. */
    uint64_t effective = signal_host_logical_mask(entry_mask);
    signal_router_forward_selected_domain(
        number, info, context, token, effective, &frame->uc_stack,
        gw_signal_interrupted_sp(frame), 0, selected);
    if (signal_router_fault_notifications()) {
      memcpy(&mask, &frame->uc_sigmask, sizeof mask);
      mask = signal_host_physical_mask(mask);
      memcpy(&frame->uc_sigmask, &mask, sizeof mask);
    }
  }
}

static struct signal_slot *host_slot(void) {
  return host_recipient;
}

/* Recipient-local maintenance, with catchable signals blocked. Registration
 * never follows foreign TLS or races with unmapping another recipient's queue.
 * An ignore generation survives a later handler registration, so old pending
 * work cannot become visible again merely because this thread was suspended.
 * Linux discards pending instances when installing SIG_IGN:
 * https://github.com/torvalds/linux/blob/v6.12/kernel/signal.c
 * See do_sigaction(), including flush_sigqueue_mask().
 */
static uint64_t synchronize_deferred_at(struct signal_slot *slot,
                                        int delivering, void *token) {
  uint64_t ignored = 0;
  if (!slot || !slot->deferred_active) {
    return 0;
  }
  uint64_t pending = 0;
  int inspected = 0;
  const int faults[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGSYS};
  for (int i = 0; i < GW_SIGNAL_FAULT_LANES; ++i) {
    int discard;
    uint64_t generation = signal_router_ignore_generation(faults[i], &discard);
    if (discard || generation != slot->deferred_ignore_generation[i]) {
      uint64_t original = UINT64_C(1) << (faults[i] - 1);
      int consumed = 0;
      if ((slot->deferred->armed & original) &&
          (gw_signal_pending_bits(&slot->deferred->pending) & original)) {
        if (!inspected &&
            syscall(SYS_rt_sigpending, &pending, sizeof pending)) {
          _exit(125);
        }
        inspected = 1;
        consumed =
            !(pending & (UINT64_C(1) << (slot->deferred->notification[i] - 1)));
        if (delivering == slot->deferred->notification[i] &&
            signal_router_token_ignore_generation(token) !=
                slot->deferred_ignore_generation[i]) {
          /* A helper selected after an ignore cannot revive the old queue.
           * A helper already selected before ignore keeps its old immutable
           * token, just as a native in-flight signal frame keeps its action. */
          consumed = 0;
        }
      }
      /* SIG_IGN discards pending signals, not data already dequeued by a
       * native reader. Its result can still be in the consumption window. */
      if (!consumed) {
        gw_signal_deferred_discard(slot->deferred, faults[i]);
      }
      slot->deferred_ignore_generation[i] = generation;
    }
    if (discard) {
      ignored |= UINT64_C(1) << (faults[i] - 1);
    }
  }
  return ignored;
}

static uint64_t synchronize_deferred(struct signal_slot *slot) {
  return synchronize_deferred_at(slot, 0, NULL);
}

int signal_host_defer(struct signal_slot *slot, const siginfo_t *information) {
  if (!slot || slot != host_slot() || !slot->deferred_active || !information ||
      information->si_signo < 1 || information->si_signo > 64) {
    return EINVAL;
  }
  uint64_t previous = gw_nonlocal_mask_block();
  uint64_t ignored = synchronize_deferred(slot);
  int result = (ignored & (UINT64_C(1) << (information->si_signo - 1)))
                   ? 0
                   : gw_signal_deferred_put(slot->deferred, information);
  synchronize_deferred(slot);
  gw_nonlocal_mask_restore(previous);
  return result;
}

uint64_t signal_carrier_mask(struct signal_slot *slot, uint64_t logical,
                             int guest) {
  if (!slot || !slot->deferred_active) {
    return logical;
  }
  if (guest) {
    logical &= ~gw_signal_fault_bits();
  }
  return gw_signal_deferred_mask(slot->deferred, logical, guest);
}

uint64_t signal_host_logical_mask(uint64_t physical) {
  const int *notifications = signal_router_fault_notifications();
  if (notifications) {
    for (int i = 0; i < GW_SIGNAL_FAULT_LANES; ++i) {
      physical &= ~(UINT64_C(1) << (notifications[i] - 1));
    }
  }
  return physical;
}

uint64_t signal_host_physical_mask(uint64_t logical) {
  if (!signal_router_fault_notifications()) {
    return logical;
  }
  struct signal_slot *slot = host_slot();
  if (slot && slot->deferred_active) {
    signal_host_prune_notifications();
    return signal_carrier_mask(slot, logical, 0);
  }
  /* Before carrier registration there can be no notification for this thread.
   * Keep the private signals blocked until its queue and lookup are published.
   */
  const int *notifications = signal_router_fault_notifications();
  for (int i = 0; i < GW_SIGNAL_FAULT_LANES; ++i) {
    logical |= UINT64_C(1) << (notifications[i] - 1);
  }
  return logical;
}

uint64_t signal_host_pending(uint64_t physical) {
  struct signal_slot *slot = host_slot();
  uint64_t previous = gw_nonlocal_mask_block();
  synchronize_deferred(slot);
  uint64_t result = slot && slot->deferred_active
                        ? gw_signal_deferred_visible(slot->deferred, physical)
                        : signal_host_logical_mask(physical);
  gw_nonlocal_mask_restore(previous);
  return result;
}

uint64_t signal_host_wait_mask(uint64_t wanted) {
  const int *notifications = signal_router_fault_notifications();
  wanted = signal_host_logical_mask(wanted);
  /* A signalfd belongs to an open file description, not its creating thread.
   * Include lanes even before registration: another recipient can read it. */
  return notifications ? gw_signal_notification_mask(notifications, wanted)
                       : wanted;
}

void signal_host_prune_notifications(void) {
  struct signal_slot *slot = host_slot();
  if (slot && slot->deferred_active) {
    uint64_t previous = gw_nonlocal_mask_block();
    synchronize_deferred(slot);
    gw_signal_deferred_prune(slot->deferred);
    gw_nonlocal_mask_restore(previous);
  }
}

int signal_host_take_notification(int number, siginfo_t *information) {
  const int *notifications = signal_router_fault_notifications();
  if (!notifications) {
    return -1;
  }
  for (int i = 0; i < GW_SIGNAL_FAULT_LANES; ++i) {
    if (notifications[i] == number) {
      struct signal_slot *slot = host_slot();
      synchronize_deferred(slot);
      return slot && slot->deferred_active
                 ? gw_signal_deferred_take(slot->deferred, number, information)
                 : 0;
    }
  }
  return -1;
}

int signal_host_collect_notification(int number, siginfo_t *information) {
  struct signal_slot *slot = host_slot();
  if (!slot || !slot->deferred_active) {
    return -1;
  }
  synchronize_deferred(slot);
  return gw_signal_deferred_collect(slot->deferred, number, information);
}

int signal_host_retirement_busy(void) {
  struct signal_slot *slot = host_slot();
  return slot && slot->deferred_active &&
         ((gw_signal_consume_busy && gw_signal_consume_busy()) ||
          slot->deferred->armed ||
          gw_signal_pending_bits(&slot->deferred->pending));
}

static int dispatch_deferred(int number, siginfo_t *info, void *context,
                             struct signal_slot *slot, uintptr_t interrupted_tp,
                             uint64_t entry_mask, void *kernel_token) {
  const int *notifications = signal_router_fault_notifications();
  if (notifications) {
    for (int i = 0; i < GW_SIGNAL_FAULT_LANES; ++i) {
      if (number != notifications[i]) {
        continue;
      }
      if (!slot || !slot->deferred_active || interrupted_tp != slot->host_tp) {
        _exit(96);
      }
      extern void signal_router_pending_observe(int) __attribute__((weak));
      if (signal_router_pending_observe) {
        signal_router_pending_observe(number);
      }
      int saved_errno = errno;
      uint64_t selection_mask = signal_router_selection_begin();
      synchronize_deferred_at(slot, number, kernel_token);
      siginfo_t original;
      int target = gw_signal_deferred_take(slot->deferred, number, &original);
      void *token = target ? kernel_token : NULL;
      int selected =
          target ? signal_router_select(target, &original, token) : 0;
      signal_router_selection_end(selection_mask);
      if (target) {
        ucontext_t *frame = context;
        uint64_t mask;
        memcpy(&mask, &frame->uc_sigmask, sizeof mask);
        mask = signal_host_logical_mask(mask);
        memcpy(&frame->uc_sigmask, &mask, sizeof mask);
        forward_host(target, &original, context, token, slot, interrupted_tp,
                     entry_mask, selected);
        memcpy(&mask, &frame->uc_sigmask, sizeof mask);
        mask = signal_carrier_mask(slot, mask, 0);
        memcpy(&frame->uc_sigmask, &mask, sizeof mask);
      }
      errno = saved_errno;
      return 1;
    }
  }
  if (slot && slot->deferred_active && interrupted_tp != slot->host_tp &&
      info->si_code <= 0 &&
      (gw_signal_fault_bits() & (UINT64_C(1) << (number - 1))) &&
      (slot->exchange->host_execution_mask & (UINT64_C(1) << (number - 1)))) {
    int saved_errno = errno;
    if (signal_host_defer(slot, info)) {
      _exit(96);
    }
    errno = saved_errno;
    return 1;
  }
  return 0;
}
#endif

/* Executed before the Host TP is restored. Must remain TLS/libc-free. */
__attribute__((no_stack_protector)) struct signal_slot *domain_signal_lookup(
    uintptr_t interrupted_tp) {
  long tid = gw_raw3(SYS_gettid, 0, 0, 0);
  struct signal_slot *dynamic = signal_dynamic_lookup(tid, interrupted_tp);
  if (dynamic) {
    return dynamic;
  }
  for (int i = 0; i < DOMAIN_FIXTURE_CARRIERS; ++i) {
    if (atomic_load_explicit(&signal_slots[i].tid, memory_order_acquire) ==
        tid) {
      return &signal_slots[i];
    }
  }
#ifdef GW_DOMAIN_CHAIN
  for (int i = 0; i <= DOMAIN_FIXTURE_CARRIERS; ++i) {
    if (atomic_load_explicit(&guest_tids[i], memory_order_acquire) == tid &&
        atomic_load_explicit(&guest_tps[i], memory_order_relaxed) ==
            interrupted_tp) {
      return (struct signal_slot *)(uintptr_t)-1;
    }
  }
#else
  (void)interrupted_tp;
#endif
  return NULL;
}

/* Host TLS and (on ARM64) x18 are valid here. Original kernel ucontext is
 * forwarded unchanged, except for the fixture's explicitly recognized fault. */
static void dispatch_core(int number, siginfo_t *info, void *context,
                          struct signal_slot *slot, uintptr_t interrupted_tp,
                          void *token, uint64_t entry_mask) {
#ifndef GW_DOMAIN_CHAIN
  (void)token;
  (void)interrupted_tp;
  (void)entry_mask;
#endif
#ifdef GW_DOMAIN_CHAIN
  if (dispatch_deferred(number, info, context, slot, interrupted_tp, entry_mask,
                        token)) {
    return;
  }
  if (slot && number == SIGUSR2 && interrupted_tp != slot->host_tp) {
    atomic_fetch_add_explicit(&domain_sr_guest_deliveries, 1,
                              memory_order_relaxed);
  }
  if (!slot) {
    forward_host(number, info, context, token, slot, interrupted_tp, entry_mask,
                 -1);
    return;
  }
#endif
  int saved_errno = errno;
  if (interrupted_tp != slot->host_tp) {
    save_guest_control(slot->exchange, context);
    if (slot->exchange->guest_signal && info->si_code > 0 &&
        (number == SIGSEGV || number == SIGBUS || number == SIGILL ||
         number == SIGFPE || number == SIGTRAP || number == SIGSYS)) {
      struct domain_signal_call call = {
          .entry = slot->exchange->guest_signal,
          .tp = interrupted_tp,
          .x18 = slot->exchange->interrupted_control[2],
          .control = {slot->exchange->interrupted_control[0],
                      slot->exchange->interrupted_control[1]},
          .number = number,
          .info = info,
          .context = context};
      if (domain_guest_signal_call(&call)) {
#ifdef GW_DOMAIN_CHAIN
        signal_router_default(number);
#else
        _exit(128 + number);
#endif
      }
      errno = saved_errno;
      return;
    }
  }
#ifdef GW_DOMAIN_TEST_SIGNALS
  atomic_fetch_add_explicit(&slot->failures, !adoption_signal_check(slot),
                            memory_order_relaxed);
#endif
  /* An asynchronous SIGSEGV is an ordinary Host-owned packet. Its signal
   * number alone does not identify a hardware fault in the Guest instruction.
   * https://github.com/torvalds/linux/blob/v6.12/kernel/signal.c
   */
  if (number == SIGSEGV && info->si_code > 0) {
    ucontext_t *uc = context;
#if defined(__x86_64__)
    uintptr_t pc = uc->uc_mcontext.gregs[REG_RIP];
#else
    uintptr_t pc = uc->uc_mcontext.pc;
#endif
    if (pc != (uintptr_t)domain_fault_instruction ||
        !slot->exchange->fault_page ||
        info->si_addr != slot->exchange->fault_page) {
#ifdef GW_DOMAIN_CHAIN
      /* An unrelated Guest fault is not a JVM implicit exception. Fail
       * closed instead of asking HotSpot to interpret a foreign stack. */
      if (interrupted_tp != slot->host_tp) {
        _exit(96);
      }
      forward_host(number, info, context, token, slot, interrupted_tp,
                   entry_mask, -1);
      errno = saved_errno;
      return;
#else
      gw_raw3(SYS_exit_group, 96, 0, 0);
      __builtin_unreachable();
#endif
    }
#if defined(__x86_64__)
    uc->uc_mcontext.gregs[REG_RIP] = (greg_t)domain_fault_resume;
#else
    uc->uc_mcontext.pc = (uintptr_t)domain_fault_resume;
#endif
    atomic_fetch_add_explicit(&slot->faults, 1, memory_order_relaxed);
  }
#ifdef GW_DOMAIN_TEST_SIGNALS
  else if (number == DOMAIN_NOTIFY_SIGNAL || number == DOMAIN_NESTED_SIGNAL) {
    atomic_fetch_add_explicit(&slot->signals, 1, memory_order_relaxed);
    if (number == DOMAIN_NOTIFY_SIGNAL) {
      /* DOMAIN_NESTED_SIGNAL is unmasked while this handler runs: nested Host
       * dispatch. */
      gw_raw3(SYS_tgkill, gw_raw3(SYS_getpid, 0, 0, 0),
              gw_raw3(SYS_gettid, 0, 0, 0), DOMAIN_NESTED_SIGNAL);
    }
  }
#endif
#ifdef GW_DOMAIN_CHAIN
  else {
    if (interrupted_tp != slot->host_tp && info->si_code > 0 &&
        (number == SIGBUS || number == SIGILL || number == SIGFPE)) {
      _exit(96);
    }
    if (interrupted_tp != slot->host_tp) {
      int index = number == SIGUSR2   ? 0
                  : number == SIGPIPE ? 1
                  : number == SIGXFSZ ? 2
                                      : -1;
      if (index >= 0) {
        ++slot->exchange->host_signal_delivered[index];
      }
      atomic_fetch_add_explicit(&domain_guest_host_signals, 1,
                                memory_order_relaxed);
    }
    forward_host(number, info, context, token, slot, interrupted_tp, entry_mask,
                 -1);
  }
#endif
  errno = saved_errno;
}

void domain_signal_dispatch(int number, siginfo_t *info, void *context,
                            struct signal_slot *slot, uintptr_t interrupted_tp,
                            void *token, uint64_t entry_mask) {
  struct gw_signal_consumption *paused =
      gw_signal_consume_interrupt ? gw_signal_consume_interrupt(number) : NULL;
  dispatch_core(number, info, context, slot, interrupted_tp, token, entry_mask);
  if (paused) {
    gw_signal_consume_resume(paused);
  }
}

void signal_install(void) {
  struct sigaction existing;
  const int owned[] = {DOMAIN_NOTIFY_SIGNAL, DOMAIN_NESTED_SIGNAL};
  for (unsigned i = 0; i < sizeof owned / sizeof owned[0]; ++i) {
    if (sigaction(owned[i], NULL, &existing) ||
        existing.sa_handler != SIG_DFL) {
      fprintf(stderr,
              "SIGNAL_SETUP occupied=%d handler=%p flags=0x%x errno=%d\n",
              owned[i], (void *)existing.sa_handler, existing.sa_flags, errno);
      _exit(97);
    }
  }
  struct sigaction action = {.sa_sigaction = domain_signal_entry,
                             .sa_flags = SA_SIGINFO | SA_ONSTACK};
  sigemptyset(&action.sa_mask);
  if (sigaction(DOMAIN_NOTIFY_SIGNAL, &action, NULL) ||
      sigaction(DOMAIN_NESTED_SIGNAL, &action, NULL)) {
    _exit(97);
  }
#ifndef GW_DOMAIN_CHAIN
  if (sigaction(SIGSEGV, &action, NULL)) {
    _exit(97);
  }
#endif
}

static int signal_try_register(struct signal_slot *slot, struct exchange *e) {
  slot->deferred_active = 0;
  slot->deferred = NULL;
#if defined(__x86_64__)
  if (gw_raw3(SYS_arch_prctl, 0x1003, (long)&slot->host_tp, 0)) {
    return -1;
  }
#else
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(slot->host_tp));
  __asm__ volatile("mov %0, x18" : "=r"(slot->host_x18));
#endif
  slot->exchange = e;
  slot->signal_stack = mmap(NULL, 65536, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (slot->signal_stack == MAP_FAILED) {
    return -1;
  }
  stack_t alternate = {.ss_sp = slot->signal_stack, .ss_size = 65536};
  if (sigaltstack(&alternate, &slot->old_stack)) {
    munmap(slot->signal_stack, 65536);
    return -1;
  }
#ifdef GW_DOMAIN_CHAIN
  const int *notifications = signal_router_fault_notifications();
  if (notifications) {
    /* Keep the ordinary carrier small. Only the explicit deferred-fault trial
     * allocates a logical queue; inactive SDK contexts pay for one pointer.
     */
    slot->deferred = mmap(NULL, sizeof *slot->deferred, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (slot->deferred == MAP_FAILED) {
      slot->deferred = NULL;
      sigaltstack(&slot->old_stack, NULL);
      munmap(slot->signal_stack, 65536);
      return -1;
    }
    if (gw_signal_deferred_init(slot->deferred, notifications)) {
      munmap(slot->deferred, sizeof *slot->deferred);
      slot->deferred = NULL;
      sigaltstack(&slot->old_stack, NULL);
      munmap(slot->signal_stack, 65536);
      return -1;
    }
    slot->deferred_active = 1;
    /* This empty queue starts in the current disposition generation. */
    memset(slot->deferred_ignore_generation, 0,
           sizeof slot->deferred_ignore_generation);
    synchronize_deferred(slot);
  }
#endif
  atomic_store_explicit(&slot->tid, e->host_tid, memory_order_release);
  sigset_t synthetic;
  sigemptyset(&synthetic);
#ifdef GW_DOMAIN_TEST_SIGNALS
  sigaddset(&synthetic, DOMAIN_NOTIFY_SIGNAL);
  sigaddset(&synthetic, DOMAIN_NESTED_SIGNAL);
#endif
  if (e->host_signal_probe) {
    /* The application exec path can inherit SIGPIPE blocked. Explicitly
     * enable delivery for this fixture, after registration, and restore the
     * entire inherited mask on exit. This is not Guest mask virtualization. */
    sigaddset(&synthetic, SIGUSR2);
    sigaddset(&synthetic, SIGPIPE);
    sigaddset(&synthetic, SIGXFSZ);
  }
  if (sigprocmask(SIG_UNBLOCK, &synthetic, &slot->inherited_mask)) {
    atomic_store_explicit(&slot->tid, 0, memory_order_release);
    sigaltstack(&slot->old_stack, NULL);
    munmap(slot->signal_stack, 65536);
    if (slot->deferred) {
      munmap(slot->deferred, sizeof *slot->deferred);
      slot->deferred = NULL;
      slot->deferred_active = 0;
    }
    return -1;
  }
#ifdef GW_DOMAIN_TEST_SIGNALS
  slot->inherited_blocked =
      sigismember(&slot->inherited_mask, DOMAIN_NOTIFY_SIGNAL) ||
      sigismember(&slot->inherited_mask, DOMAIN_NESTED_SIGNAL);
#else
  slot->inherited_blocked = 0;
#endif
  if (slot->deferred_active) {
    atomic_fetch_add_explicit(&deferred_recipients, 1, memory_order_release);
  }
  return 0;
}

void signal_register(struct signal_slot *slot, struct exchange *e) {
  if (signal_try_register(slot, e)) {
    _exit(98);
  }
  publish_recipient(slot);
}

int signal_prepare_release(struct signal_slot *slot, int thread_exiting) {
#ifdef GW_DOMAIN_CHAIN
  if (slot && slot->deferred_active) {
    if (!thread_exiting && slot == host_slot() && gw_signal_consume_busy &&
        gw_signal_consume_busy()) {
      return EBUSY;
    }
    uint64_t mask = gw_nonlocal_mask_block();
    synchronize_deferred(slot);
    gw_signal_deferred_prune(slot->deferred);
    if (thread_exiting) {
      uint64_t notifications = gw_signal_deferred_notifications(slot->deferred);
      struct timespec zero = {0};
      siginfo_t information, original;
      int saved_errno = errno, number;
      while ((number = (int)syscall(SYS_rt_sigtimedwait, &notifications,
                                    &information, &zero,
                                    sizeof notifications)) > 0) {
        gw_signal_deferred_take(slot->deferred, number, &original);
      }
      int error = errno;
      errno = saved_errno;
      if (number != -1 || error != EAGAIN) {
        gw_nonlocal_mask_restore(mask);
        return error ? error : EIO;
      }
      if (!slot->deferred->armed) {
        uint64_t pending = gw_signal_pending_bits(&slot->deferred->pending);
        while (pending) {
          int original = __builtin_ctzll(pending) + 1;
          gw_signal_deferred_discard(slot->deferred, original);
          pending &= pending - 1;
        }
      }
    }
    int result = slot->deferred->armed ||
                         gw_signal_pending_bits(&slot->deferred->pending)
                     ? EBUSY
                     : 0;
    gw_nonlocal_mask_restore(mask);
    return result;
  }
#else
  (void)slot;
  (void)thread_exiting;
#endif
  return 0;
}

void signal_unregister(struct signal_slot *slot) {
#ifdef GW_DOMAIN_CHAIN
  if (slot->deferred_active) {
    /* This integration trial requires draining before teardown. Do not revoke
     * a recipient while a kernel notification can still name its old queue.
     * Retention beyond carrier teardown is a separate acceptance obligation.
     */
    uint64_t mask = gw_nonlocal_mask_block();
    if (gw_signal_deferred_close(slot->deferred)) {
      _exit(98);
    }
    slot->deferred_active = 0;
    atomic_fetch_sub_explicit(&deferred_recipients, 1, memory_order_release);
    gw_nonlocal_mask_restore(mask);
  }
#endif
  /* Senders have quiesced. Block/drain only our synthetic asynchronous
   * signals before revoking the registry entry. Never mask hardware faults. */
  sigset_t synthetic;
  sigemptyset(&synthetic);
#ifdef GW_DOMAIN_TEST_SIGNALS
  sigaddset(&synthetic, DOMAIN_NOTIFY_SIGNAL);
  sigaddset(&synthetic, DOMAIN_NESTED_SIGNAL);
#endif
  if (sigprocmask(SIG_BLOCK, &synthetic, NULL)) {
    _exit(98);
  }
  struct timespec zero = {0};
  while (sigtimedwait(&synthetic, NULL, &zero) >= 0) {
  }
  atomic_store_explicit(&slot->tid, 0, memory_order_release);
  if (host_recipient == slot) {
    host_recipient = NULL;
  }
  if (slot->deferred) {
    munmap(slot->deferred, sizeof *slot->deferred);
    slot->deferred = NULL;
  }
  if (sigaltstack(&slot->old_stack, NULL)) {
    _exit(98);
  }
  munmap(slot->signal_stack, 65536);
#ifndef GW_DOMAIN_TEST_SIGNALS
  /* Production registration does not alter the Host mask. Do not roll back
   * legitimate changes made by the application during the carrier lifetime. */
  if (!slot->exchange->host_signal_probe) {
    return;
  }
#endif
  if (sigprocmask(SIG_SETMASK, &slot->inherited_mask, NULL)) {
    _exit(98);
  }
  sigset_t restored;
  if (sigprocmask(SIG_SETMASK, NULL, &restored)) {
    _exit(98);
  }
  /* Compare membership rather than padding in libc-specific sigset_t. */
  for (int number = 1; number < NSIG; ++number) {
    atomic_fetch_add_explicit(&slot->failures,
                              sigismember(&restored, number) !=
                                  sigismember(&slot->inherited_mask, number),
                              memory_order_relaxed);
  }
}

void signal_send(void) {
  long pid = syscall(SYS_getpid);
  for (int i = 0; i < DOMAIN_FIXTURE_CARRIERS; ++i) {
    long tid = atomic_load_explicit(&signal_slots[i].tid, memory_order_acquire);
    if (tid) {
      syscall(SYS_tgkill, pid, tid, DOMAIN_NOTIFY_SIGNAL);
    }
  }
}
