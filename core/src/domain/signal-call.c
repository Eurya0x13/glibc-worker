#define _GNU_SOURCE
#include "domain.h"
#include "signal-stack.h"

struct signal_stack_call {
  const struct domain_signal_call *call;
  int result;
};

static void on_guest_stack(void *argument) {
  struct signal_stack_call *scope = argument;
  scope->result = domain_guest_signal_call(scope->call);
}

int domain_guest_signal_stack_call(const struct domain_signal_call *call,
                                   uintptr_t top) {
  struct signal_stack_call scope = {.call = call};
  domain_stack_call(on_guest_stack, &scope, top);
  return scope.result;
}
