#ifndef GW_SIGNAL_TARGETS_H
#define GW_SIGNAL_TARGETS_H
#include "signal-inbox.h"
#include <stddef.h>

struct gw_signal_targets;

/* Stable identity, independent of an allocator-reused libc descriptor/TID.
 * Close revokes sends under the same registry transaction as notification.
 * Pins belong to entries/saved continuations, not to kernel thread counts. */
struct gw_signal_target {
  struct gw_signal_receiver receiver;
  struct gw_signal_targets *registry;
  uintptr_t key;
  int tid;
  void *context;
  size_t index, pins;
};

typedef int (*gw_signal_target_notify)(void *, struct gw_signal_target *);

/* Compiled/executed by the owning libc. Every call requires all catchable
 * signals physically blocked, including sends from a libc send hook. Callback
 * runs under both writers: nonblocking notification only, no registry calls. */
struct gw_signal_targets {
  atomic_flag writer;
  struct gw_signal_inbox inbox;
  struct gw_signal_target **slots, **active;
  size_t capacity, occupied, count, active_capacity, retained;
  gw_signal_target_notify notify;
  void *context;
  int closed, wake_error;
};

void gw_signal_targets_init(struct gw_signal_targets *, uint64_t, uint64_t,
                            gw_signal_target_notify, void *);
int gw_signal_target_open(struct gw_signal_targets *, uintptr_t, int, uint64_t,
                          void *, struct gw_signal_target **);
struct gw_signal_publication gw_signal_targets_send(struct gw_signal_targets *,
                                                    uintptr_t, int,
                                                    const siginfo_t *);
struct gw_signal_publication gw_signal_target_mask(struct gw_signal_target *,
                                                   uint64_t);
int gw_signal_target_deliver(struct gw_signal_target *, siginfo_t *,
                             enum gw_signal_queue_source *);
uint64_t gw_signal_target_pending(struct gw_signal_target *);
int gw_signal_target_pin(struct gw_signal_target *);
/* Derive another continuation hold from a pin the caller already owns.
 * Unlike new-entry admission, this remains possible after send revocation. */
int gw_signal_target_retain(struct gw_signal_target *);
int gw_signal_target_unpin(struct gw_signal_target *);
void gw_signal_target_close(struct gw_signal_target *);
/* Explicit owner quiescence: after source revocation and all selected/saved
 * entry frames have gone. EBUSY retains storage while a continuation is pinned.
 */
int gw_signal_target_release(struct gw_signal_target *);
int gw_signal_targets_retry(struct gw_signal_targets *);
int gw_signal_targets_error(struct gw_signal_targets *);
void gw_signal_targets_close(struct gw_signal_targets *);
int gw_signal_targets_destroy(struct gw_signal_targets *);
#endif
