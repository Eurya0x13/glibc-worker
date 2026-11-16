/* Guest-only dispositions. No Host libc function pointer is exposed here. */
#ifdef GW_DOMAIN_NONLOCAL
#define _GNU_SOURCE
#include "domain_signals.h"
#include "nonlocal-mask.h"
#include "signal-stack.h"
#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>

struct guest_action {
  struct sigaction action;
  _Atomic int fired;
};

static _Atomic(struct guest_action *) actions[65];
static atomic_flag writer = ATOMIC_FLAG_INIT;

static _Noreturn void guest_default(int number) {
  /* Linux LP64 kernel sigaction ABI, independent of either libc's public
   * struct sigaction. No restorer is needed for a default disposition.
   * https://github.com/torvalds/linux/blob/v6.12/include/uapi/asm-generic/signal.h
   * https://github.com/torvalds/linux/blob/v6.12/arch/x86/include/uapi/asm/signal.h
   */
  const struct {
    uintptr_t handler, flags, restorer;
    uint64_t mask;
  } action = {0};

  uint64_t bit = UINT64_C(1) << (number - 1);
  if (syscall(SYS_rt_sigaction, number, &action, NULL, sizeof bit) ||
      syscall(SYS_rt_sigprocmask, SIG_UNBLOCK, &bit, NULL, sizeof bit) ||
      syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), number)) {
    _exit(126);
  }
  _exit(128 + number);
}

static int synchronous_number(int number) {
  return number == SIGSEGV || number == SIGBUS || number == SIGILL ||
         number == SIGFPE || number == SIGTRAP || number == SIGSYS;
}

int gw_guest_sigaction(int number, const void *input, void *output,
                       int *handled) {
  if (!synchronous_number(number)) {
    *handled = 0;
    return 0;
  }
  *handled = 1;
  const struct sigaction *action = input;
  struct sigaction *previous = output;
  uint64_t mask = gw_nonlocal_mask_block();
  while (atomic_flag_test_and_set_explicit(&writer, memory_order_acquire)) {
  }
  struct guest_action *old =
      atomic_load_explicit(&actions[number], memory_order_acquire);
  struct guest_action *next = NULL;
  if (action) {
    next = (void *)syscall(SYS_mmap, NULL, sizeof *next, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (next == MAP_FAILED) {
      int error = errno;
      atomic_flag_clear_explicit(&writer, memory_order_release);
      gw_nonlocal_mask_restore(mask);
      errno = error;
      return -1;
    }
    next->action = *action;
    atomic_init(&next->fired, 0);
  }
  if (previous) {
    *previous = old ? old->action : (struct sigaction){.sa_handler = SIG_DFL};
    if (old && (previous->sa_flags & SA_RESETHAND) &&
        atomic_load_explicit(&old->fired, memory_order_acquire)) {
      previous->sa_handler = SIG_DFL;
    }
  }
  if (action) {
    /* Immutable generations remain pinned until recipient/continuation
     * quiescence is implemented. An old delivery may already hold old. */
    atomic_store_explicit(&actions[number], next, memory_order_release);
  }
  atomic_flag_clear_explicit(&writer, memory_order_release);
  gw_nonlocal_mask_restore(mask);
  return 0;
}

struct guest_delivery {
  const struct sigaction *action;
  int number;
  void *information, *frame;
  uint64_t mask;
};

static void guest_delivery_run(void *argument) {
  struct guest_delivery *delivery = argument;
  gw_nonlocal_mask_restore(delivery->mask);
  if (delivery->action->sa_flags & SA_SIGINFO) {
    delivery->action->sa_sigaction(delivery->number, delivery->information,
                                   delivery->frame);
  } else {
    delivery->action->sa_handler(delivery->number);
  }
  gw_nonlocal_mask_block();
}

int gw_guest_signal_dispatch(int number, void *information, void *frame,
                             uint64_t *logical, uint64_t carrier,
                             uint64_t reserved) {
  ucontext_t *context = frame;
  uint64_t interrupted, action_mask;
  memcpy(&interrupted, &context->uc_sigmask, sizeof interrupted);
  interrupted = (interrupted & ~reserved) | (*logical & reserved);
  uint64_t bit = UINT64_C(1) << (number - 1);
  /* Forced hardware faults also reset a blocked disposition. A suspended
   * Host may have this signal unblocked physically while Guest blocks it
   * logically, so the kernel alone cannot enforce this Guest rule. */
  if (synchronous_number(number) && ((siginfo_t *)information)->si_code > 0 &&
      (interrupted & bit)) {
    guest_default(number);
  }
  struct guest_action *record =
      atomic_load_explicit(&actions[number], memory_order_acquire);
  if (!record || record->action.sa_handler == SIG_DFL) {
    guest_default(number);
  }
  if (record->action.sa_handler == SIG_IGN) {
    /* Linux force_sig_info_to_task resets an ignored synchronous hardware
     * fault to SIG_DFL. The outer kernel route hides Guest SIG_IGN, so apply
     * the same rule here instead of retrying the fault forever.
     * https://github.com/torvalds/linux/blob/v6.12/kernel/signal.c */
    if (synchronous_number(number) && ((siginfo_t *)information)->si_code > 0) {
      guest_default(number);
    }
    return 0;
  }
  /* Native reset-on-delivery changes the handler only. Retain this immutable
   * generation's flags/mask for queries, and consume only this generation if
   * a registration races with delivery on another recipient.
   * https://github.com/torvalds/linux/blob/v6.12/kernel/signal.c */
  if ((record->action.sa_flags & SA_RESETHAND) &&
      atomic_exchange_explicit(&record->fired, 1, memory_order_acq_rel)) {
    guest_default(number);
  }
  /* A Guest handler may inspect/modify this mask, or pass this frame to its
   * native setcontext. Expose the interrupted logical mask, then translate
   * only the mask back for the kernel's eventual sigreturn.
   * https://github.com/torvalds/linux/blob/v6.12/arch/arm64/kernel/signal.c */
  memcpy(&context->uc_sigmask, &interrupted, sizeof interrupted);
  memcpy(&action_mask, &record->action.sa_mask, sizeof action_mask);
  uint64_t effective = interrupted | action_mask;
  if (!(record->action.sa_flags & SA_NODEFER)) {
    effective |= bit;
  }
  uint64_t kernel = (effective & ~reserved) | (carrier & reserved);
  kernel = (kernel & ~bit) | (effective & bit);
  uint64_t physical = gw_nonlocal_mask_block();
  *logical = effective;
  uintptr_t top = 0, interrupted_sp = gw_signal_interrupted_sp(context);
  if (!(record->action.sa_flags & SA_ONSTACK) &&
      gw_stack_contains(&context->uc_stack, (uintptr_t)&physical) &&
      !gw_stack_contains(&context->uc_stack, interrupted_sp)) {
    /* Kernel SA_ONSTACK belongs to the outer route, while this logical Guest
     * action chose the ordinary stack. Keep its kernel frame on the alternate
     * stack, and invoke the handler below the interrupted stack's red zone. */
    top = gw_signal_stack_top(interrupted_sp);
  }
  struct guest_delivery delivery = {&record->action, number, information, frame,
                                    kernel};
  domain_stack_call(guest_delivery_run, &delivery, top);
  memcpy(&interrupted, &context->uc_sigmask, sizeof interrupted);
  *logical = interrupted;
  kernel = (interrupted & ~reserved) | (carrier & reserved);
  memcpy(&context->uc_sigmask, &kernel, sizeof kernel);
  gw_nonlocal_mask_restore(physical);
  return 0;
}
#endif
