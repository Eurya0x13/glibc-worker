#ifndef GW_SIGNAL_ACTIONS_H
#define GW_SIGNAL_ACTIONS_H
#include "signal-targets.h"

/* A logical action is copied under the writer before application delivery.
 * No entry points into this mutable table, so replacing an action needs no
 * retired allocation or handler-count approximation of frame quiescence. */
struct gw_signal_action {
  struct sigaction action;
  uint64_t revision;
};

struct gw_signal_actions {
  atomic_flag writer;
  struct gw_signal_targets *targets;
  struct gw_signal_target *representative;
  struct gw_signal_action current[65]; /* Linux LP64 signal-number ABI. */
  uint64_t reserved, revision;
  int closed;
};

struct gw_signal_delivery {
  struct gw_signal_action selected;
  siginfo_t information;
  enum gw_signal_queue_source source;
};

/* Compile with the owning libc and physically block catchable signals before
 * every operation. Lock order is actions -> targets -> inbox. User handlers
 * run only after releasing all writers. A NULL targets pointer is sufficient
 * for independent synchronous dispositions without pending queues. */
void gw_signal_actions_init(struct gw_signal_actions *,
                            struct gw_signal_targets *, uint64_t reserved);
/* Bootstrap before process sends. Hold the logical group leader's identity
 * and mask through its exit, matching Linux's process-send ignore filter. */
int gw_signal_actions_representative(struct gw_signal_actions *,
                                     struct gw_signal_target *);
int gw_signal_actions_change(struct gw_signal_actions *, int,
                             const struct sigaction *, struct sigaction *);
/* Select an already identified signal, consuming only its one-shot action.
 * The snapshot remains valid after registration changes; no table pointer
 * escapes. A later native continuation owns its own snapshot storage. */
int gw_signal_actions_select(struct gw_signal_actions *, int,
                             struct gw_signal_action *);
/* Queue and choose the disposition under the same outer transaction. Ignored
 * sends still validate their target. Blocked ignored signals remain pending;
 * only unblocked ignored sends avoid quota. Process sends require an explicit
 * representative; no arbitrary dense-vector member substitutes for it. */
struct gw_signal_publication gw_signal_actions_send(struct gw_signal_actions *,
                                                    uintptr_t, int,
                                                    const siginfo_t *);
/* Positive signal number, zero when empty, negative error on invalid state. */
int gw_signal_actions_deliver(struct gw_signal_actions *,
                              struct gw_signal_target *,
                              struct gw_signal_delivery *);
void gw_signal_actions_close(struct gw_signal_actions *);
#endif
