/* Guest-only dispositions. No Host libc function pointer is exposed here. */
#ifdef GW_DOMAIN_NONLOCAL
#define _GNU_SOURCE
#include "domain_signals.h"
#include "domain_signal_workers.h"
#include "nonlocal-mask.h"
#include "signal-stack.h"
#include "signal-actions.h"
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <ucontext.h>

struct gw_signal_actions gw_guest_actions;

void gw_guest_signals_init(struct gw_signal_targets *targets,
                           uint64_t reserved) {
  gw_signal_actions_init(&gw_guest_actions, targets, reserved);
}

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
  /* Public GNU sigaction rejects its private 63/64 channels. The internal
   * registration entry still owns their real cancellation and setxid state. */
  if (number == SIGKILL || number == SIGSTOP || number == 63 || number == 64 ||
      (!synchronous_number(number) && !gw_guest_actions.targets)) {
    *handled = 0;
    return 0;
  }
  *handled = 1;
  int saved = errno;
  uint64_t mask = gw_nonlocal_mask_block();
  int error =
      gw_signal_actions_change(&gw_guest_actions, number, input, output);
  errno = error ? error : saved;
  gw_nonlocal_mask_restore(mask);
  return error ? -1 : 0;
}

struct guest_delivery {
  const struct sigaction *action;
  int number;
  void *information, *frame;
  uint64_t mask;
  int disarm;
};

static void guest_delivery_run(void *argument) {
  struct guest_delivery *delivery = argument;
  if (delivery->disarm) {
    gw_signal_stack_set(&(stack_t){.ss_flags = SS_DISABLE});
  }
  gw_nonlocal_mask_restore(delivery->mask);
  if (delivery->action->sa_flags & SA_SIGINFO) {
    delivery->action->sa_sigaction(delivery->number, delivery->information,
                                   delivery->frame);
  } else {
    delivery->action->sa_handler(delivery->number);
  }
  gw_nonlocal_mask_block();
}

static uint64_t logical_mask(const ucontext_t *context,
                             struct gw_guest_signal_state state) {
  uint64_t physical;
  memcpy(&physical, &context->uc_sigmask, sizeof physical);
  return (physical & ~state.virtual_mask) |
         (*state.logical & state.virtual_mask);
}

static uint64_t physical_mask(uint64_t logical,
                              struct gw_guest_signal_state state) {
  return (logical & ~state.virtual_mask) | (state.carrier & state.virtual_mask);
}

static void publish_mask(uint64_t logical, struct gw_guest_signal_state state) {
  *state.logical = logical;
  if (state.target) {
    struct gw_signal_publication result =
        gw_signal_target_mask(state.target, logical);
    if (result.status || result.wake_error) {
      /* A live worker owns its target until its NPTL exit callback. Wake
       * failure is observed at the owning runtime boundary. */
      gw_guest_workers_wake_error(result.status ? result.status
                                                : result.wake_error);
    }
  }
}

static uintptr_t delivery_top(const struct sigaction *action,
                              const ucontext_t *context, uintptr_t current_sp,
                              uintptr_t stack_floor, int *disarm) {
  uintptr_t interrupted_sp = gw_signal_interrupted_sp(context);
  const stack_t *alternate = &context->uc_stack;
  int interrupted_on_alternate = gw_stack_contains(alternate, interrupted_sp);
  int floor_on_alternate = gw_stack_contains(alternate, stack_floor);
  uintptr_t resume_top = stack_floor && stack_floor < interrupted_sp &&
                                 floor_on_alternate == interrupted_on_alternate
                             ? stack_floor
                             : interrupted_sp;
  if (interrupted_on_alternate) {
    return stack_floor ? gw_signal_stack_top(resume_top) : 0;
  }
  int on_alternate = gw_stack_contains(alternate, current_sp);
  if (action->sa_flags & SA_ONSTACK) {
    if (!on_alternate && !(alternate->ss_flags & SS_DISABLE)) {
      *disarm = !!((unsigned)alternate->ss_flags & GW_SS_AUTODISARM);
      return floor_on_alternate
                 ? gw_signal_stack_top(stack_floor)
                 : (uintptr_t)alternate->ss_sp + alternate->ss_size;
    }
  } else if (on_alternate) {
    /* The outer synchronous route may already have put the kernel frame on
     * the alternate stack. Respect an ordinary-stack Guest action. */
    return gw_signal_stack_top(interrupted_sp);
  }
  return stack_floor ? gw_signal_stack_top(resume_top) : 0;
}

static void dispatch_action(int number, void *information, ucontext_t *context,
                            struct gw_guest_signal_state state,
                            const struct sigaction *action, int synchronous) {
  uint64_t interrupted, action_mask;
  interrupted = logical_mask(context, state);
  uint64_t bit = UINT64_C(1) << (number - 1);
  if (action->sa_handler == SIG_DFL) {
    if (!synchronous && (number == SIGCHLD || number == SIGURG ||
                         number == SIGWINCH || number == SIGCONT)) {
      return;
    }
    guest_default(number);
  }
  if (action->sa_handler == SIG_IGN) {
    /* Linux forces an ignored synchronous hardware fault back to default.
     * https://github.com/torvalds/linux/blob/v6.12/kernel/signal.c */
    if (synchronous) {
      guest_default(number);
    }
    return;
  }
  /* A Guest handler may inspect/modify this mask, or pass this frame to its
   * native setcontext. Expose the interrupted logical mask, then translate
   * only the mask back for the kernel's eventual sigreturn.
   * https://github.com/torvalds/linux/blob/v6.12/arch/arm64/kernel/signal.c */
  memcpy(&context->uc_sigmask, &interrupted, sizeof interrupted);
  memcpy(&action_mask, &action->sa_mask, sizeof action_mask);
  uint64_t effective = interrupted | action_mask;
  if (!(action->sa_flags & SA_NODEFER)) {
    effective |= bit;
  }
  uint64_t kernel = physical_mask(effective, state);
  if (synchronous) {
    kernel = (kernel & ~bit) | (effective & bit);
  }
  uint64_t physical = gw_nonlocal_mask_block();
  publish_mask(effective, state);
  if (state.target) {
    gw_signal_target_note_delivery(state.target);
  }

  struct guest_delivery delivery = {action,  number, information,
                                    context, kernel, 0};
  uintptr_t top = delivery_top(action, context, (uintptr_t)&physical,
                               state.stack_floor, &delivery.disarm);
  domain_stack_call(guest_delivery_run, &delivery, top, NULL);

  memcpy(&interrupted, &context->uc_sigmask, sizeof interrupted);
  publish_mask(interrupted, state);
  kernel = physical_mask(interrupted, state);
  memcpy(&context->uc_sigmask, &kernel, sizeof kernel);
  gw_nonlocal_mask_restore(physical);
}

int gw_guest_signal_dispatch(int number, void *information, void *frame,
                             struct gw_guest_signal_state state) {
  int synchronous =
      synchronous_number(number) && ((siginfo_t *)information)->si_code > 0;
  /* The physical mask can leave a logically blocked hardware fault enabled.
   * Preserve the fault's native forced default action in that case. */
  if (synchronous &&
      (logical_mask(frame, state) & (UINT64_C(1) << (number - 1)))) {
    guest_default(number);
  }
  struct gw_signal_action selected;
  if (gw_signal_actions_select(&gw_guest_actions, number, &selected)) {
    return EINVAL;
  }
  dispatch_action(number, information, frame, state, &selected.action,
                  synchronous);
  return 0;
}

void gw_guest_signal_deliver(const struct gw_signal_delivery *delivery,
                             void *frame, struct gw_guest_signal_state state) {
  siginfo_t information = delivery->information;
  dispatch_action(information.si_signo, &information, frame, state,
                  &delivery->selected.action, 0);
}
#endif
