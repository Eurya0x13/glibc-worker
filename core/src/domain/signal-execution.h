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

enum gw_signal_exit {
  GW_SIGNAL_RETURN = 0,
  GW_SIGNAL_HOST_JUMP,
  GW_SIGNAL_GUEST_JUMP
};

/* One entered native signal invocation. Storage, both guarded stacks and the
 * owning TCB must survive entry and destructive recovery. Do not recycle an
 * invocation while an application continuation can still resume into it. */
struct gw_signal_execution {
  struct domain host, guest, abort;
  struct gw_signal_execution *previous;
  void (*swap)(struct domain *, struct domain *);
  int (*invoke)(void *);
  void (*finish)(void *, enum gw_signal_exit);
  void (*release_host)(void *);
  void *argument, *landing;
  uint64_t *carrier_mask;
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
  void (*finish)(void *, enum gw_signal_exit);
  void (*release_host)(void *);
  void *argument;
  uint64_t *carrier_mask;
};

struct gw_signal_execution_entries {
  uintptr_t start;
  void (*run)(void *), (*abandon)(void *);
};

/* Use owner-compiled entry addresses with an actual suspended Guest state.
 * This initializer copies the record. It makes no foreign libc calls. */
void gw_guest_signal_entries(struct gw_signal_execution_entries *);
int gw_signal_execution_from_saved(struct gw_signal_execution *,
                                   const struct gw_signal_execution_config *,
                                   const struct domain *,
                                   const struct gw_signal_execution_entries *);

/* Compile signal-execution-guest.c and start.S with the owning Guest libc.
 * Initialize while its valid TCB/FP state is installed and catchable signals
 * are blocked. finish runs once, also blocked, after return or abandonment.
 * It releases bridge resources, not arbitrary application-owned resources. */
int gw_guest_signal_execution_init(struct gw_signal_execution *,
                                   const struct gw_signal_execution_config *);
int gw_guest_signal_internal_jump(const void *);
_Noreturn void gw_guest_signal_escape(void);

/* Host carrier with a registered Guest TCB and capture/restore interposition.
 * Every transition is masked. A Host-target native jump retires Guest work
 * through its own checkpoint, then resumes the requested Host continuation.
 * After entry starts, release_host runs once on Host after either outcome.
 * This is destructive recovery, not a resumable-ucontext lifetime protocol. */
int signal_guest_execute(struct gw_signal_execution *, void *host_frame);
#endif
