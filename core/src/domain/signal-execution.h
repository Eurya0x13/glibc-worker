#ifndef GW_SIGNAL_EXECUTION_H
#define GW_SIGNAL_EXECUTION_H
#include "domain_context.h"
#include <signal.h>
#include <stddef.h>

enum gw_signal_execution_phase {
  GW_SIGNAL_EXECUTION_PREPARED = 1,
  GW_SIGNAL_EXECUTION_RUNNING,
  GW_SIGNAL_EXECUTION_DONE,
  GW_SIGNAL_EXECUTION_ABORTED
};

/* One entered native signal invocation. Storage, both guarded stacks and the
 * owning TCB must survive entry and destructive recovery. Do not recycle an
 * invocation while an application continuation can still resume into it. */
struct gw_signal_execution {
  struct domain host, guest, abort;
  void (*swap)(struct domain *, struct domain *);
  int (*invoke)(void *);
  void (*finish)(void *, int abandoned);
  void *argument, *landing;
  stack_t alternate;
  unsigned version;
  volatile sig_atomic_t phase;
  int result;
};

struct gw_signal_execution_config {
  void *stack, *abort_stack;
  size_t stack_size, abort_stack_size;
  stack_t alternate;
  void (*swap)(struct domain *, struct domain *);
  int (*invoke)(void *);
  void (*finish)(void *, int abandoned);
  void *argument;
};

/* Compile signal-execution-guest.c and start.S with the owning Guest libc.
 * Initialize while its valid TCB/FP state is installed and catchable signals
 * are blocked. finish runs once, also blocked, after return or abandonment.
 * It releases bridge resources, not arbitrary application-owned resources. */
int gw_guest_signal_execution_init(struct gw_signal_execution *,
                                   const struct gw_signal_execution_config *);

/* Host carrier with a registered Guest TCB and capture/restore interposition.
 * Every transition is masked. A Host-target native jump retires Guest work
 * through its own checkpoint, then resumes the requested Host continuation.
 * This is destructive recovery, not a resumable-ucontext lifetime protocol. */
int signal_guest_execute(struct gw_signal_execution *, void *host_frame);
#endif
