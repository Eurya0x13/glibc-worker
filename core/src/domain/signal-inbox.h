#ifndef GW_SIGNAL_INBOX_H
#define GW_SIGNAL_INBOX_H
#include "signal-pending.h"

struct gw_signal_receiver;

/* One domain, independent of the other libc's kernel pending queues. The
 * owner physically blocks catchable signals for every operation, including
 * send/close. A virtual Guest mask alone does not establish this condition.
 * The outer lock linearizes private/shared selection, mask changes and quota.
 * Storage must outlive senders, recipients and saved signal continuations. */
struct gw_signal_inbox {
  atomic_flag lock;
  struct gw_signal_pending shared;
  uint64_t reserved, limit, queued;
  unsigned receivers;
  int closed;
};

/* Stable registration owned by the integration's thread registry. This layer
 * never treats a cached kernel TID or a libc pthread_t as a lifetime token.
 * Reuse/destroy only after sender and saved-continuation quiescence. */
struct gw_signal_receiver {
  struct gw_signal_inbox *owner;
  struct gw_signal_pending directed;
  uint64_t blocked;
  int closed;
};

/* Called under the inbox lock, with catchable signals blocked on the sender.
 * Only issue a nonblocking notification; do not call application code or any
 * inbox API. NULL target means shared pending: the integration selects a live
 * eligible Guest recipient, or retains it if all Guest recipients block it.
 * Returning an error does NOT retract an already accepted publication. */
typedef int (*gw_signal_inbox_notify)(void *, struct gw_signal_receiver *);

struct gw_signal_publication {
  int status;     /* Zero: accepted/coalesced. Otherwise nothing was queued. */
  int wake_error; /* Accepted but notification failed: retry or quarantine. */
};

enum gw_signal_queue_source {
  GW_SIGNAL_NONE,
  GW_SIGNAL_THREAD,
  GW_SIGNAL_PROCESS
};

/* Initialize fresh, zeroed storage before admitting senders. The limit is an
 * aggregate domain RT budget, not an emulation of per-UID RLIMIT_SIGPENDING.
 * A kernel notification has its own errors and resource accounting. */
void gw_signal_inbox_init(struct gw_signal_inbox *, uint64_t reserved,
                          uint64_t realtime_limit);
int gw_signal_receiver_open(struct gw_signal_inbox *,
                            struct gw_signal_receiver *, uint64_t blocked);
struct gw_signal_publication gw_signal_inbox_put(struct gw_signal_inbox *,
                                                 struct gw_signal_receiver *,
                                                 const siginfo_t *,
                                                 gw_signal_inbox_notify,
                                                 void *);
int gw_signal_receiver_mask(struct gw_signal_receiver *, uint64_t blocked,
                            gw_signal_inbox_notify, void *);
int gw_signal_inbox_take(struct gw_signal_receiver *, uint64_t eligible,
                         siginfo_t *, enum gw_signal_queue_source *);
/* Handler selection samples the receiver mask in the selection transaction,
 * including after a previously issued notification has become stale. */
int gw_signal_inbox_deliver(struct gw_signal_receiver *, siginfo_t *,
                            enum gw_signal_queue_source *);
uint64_t gw_signal_inbox_pending(struct gw_signal_receiver *);
/* Discard one queue's pending instances and release its aggregate RT quota.
 * NULL receiver selects the shared queue. A registry-level disposition change
 * serializes this with every directed queue and future sends. No notification
 * is issued or retracted: already selected kernel helpers can be stale. */
int gw_signal_inbox_discard(struct gw_signal_inbox *,
                            struct gw_signal_receiver *, int);
void gw_signal_receiver_close(struct gw_signal_receiver *);
void gw_signal_inbox_close(struct gw_signal_inbox *);
/* These are quiescent operations. EBUSY prevents destroying a group with live
 * receivers. Never infer continuation quiescence from the receiver count. */
int gw_signal_receiver_destroy(struct gw_signal_receiver *);
int gw_signal_inbox_destroy(struct gw_signal_inbox *);
#endif
