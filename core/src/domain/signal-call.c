#define _GNU_SOURCE
#include "domain.h"
#include "signal-stack.h"

struct signal_stack_call {
  const struct domain_signal_call *call;
  const stack_t *alternate;
  int configured;
  int result;
};

static void on_guest_stack(void *argument) {
  struct signal_stack_call *scope = argument;
  if (scope->alternate) {
    /* SP has left Host's live routing frame. Changing the physical alternate
     * stack earlier would fail with EPERM, or let a nested kernel frame
     * overwrite that still-live Host frame.
     * https://github.com/torvalds/linux/blob/v6.12/kernel/signal.c
     * See do_sigaltstack() and on_sig_stack(). */
    int saved = errno;
    if (syscall(SYS_sigaltstack, scope->alternate, NULL)) {
      scope->result = errno;
      errno = saved;
      return;
    }
    scope->configured = 1;
    errno = saved;
  }
  scope->result = domain_guest_signal_call(scope->call);
}

int domain_guest_signal_stack_call(const struct domain_signal_call *call,
                                   uintptr_t top) {
  struct signal_stack_call scope = {.call = call};
  domain_stack_call(on_guest_stack, &scope, top);
  return scope.result;
}

int domain_guest_signal_stack_enter(const struct domain_signal_call *call,
                                    const stack_t *alternate,
                                    uintptr_t *suspended_sp) {
  if (!call || !alternate || !alternate->ss_sp || alternate->ss_flags ||
      !suspended_sp || alternate->ss_size < 16384 ||
      alternate->ss_size > UINTPTR_MAX - (uintptr_t)alternate->ss_sp) {
    return EINVAL;
  }
  stack_t previous;
  gw_signal_stack_get(&previous);
  struct signal_stack_call scope = {.call = call, .alternate = alternate};
  uintptr_t top = (uintptr_t)alternate->ss_sp + alternate->ss_size;
  domain_stack_call_record(on_guest_stack, &scope, top, suspended_sp);
  if (scope.configured) {
    /* SP is back on Host before changing away from Guest's routing stack. */
    gw_signal_stack_set(&previous);
  }
  return scope.result;
}
