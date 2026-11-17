#ifndef GW_SIGNAL_PENDING_H
#define GW_SIGNAL_PENDING_H
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>

struct gw_signal_node;
struct gw_signal_chunk;

/* The owner blocks catchable signals before every operation. This makes the
 * short writer lock usable by a handler on the same recipient: it cannot
 * interrupt a writer holding the lock. No allocator or callback runs under it.
 * Objects are owned by one libc; only siginfo values cross a domain boundary.
 */
struct gw_signal_pending {
  atomic_flag lock;
  _Atomic uint64_t present;
  siginfo_t standard[32];
  struct gw_signal_node *first[65], *last[65], *free;
  struct gw_signal_chunk *chunks;
  uint64_t queued;
  int closed;
};

int gw_signal_pending_put(struct gw_signal_pending *, const siginfo_t *,
                          uint64_t realtime_limit);
int gw_signal_pending_take(struct gw_signal_pending *, uint64_t eligible,
                           siginfo_t *);
int gw_signal_pending_peek(struct gw_signal_pending *, uint64_t eligible,
                           siginfo_t *);
uint64_t gw_signal_pending_bits(const struct gw_signal_pending *);
/* Linux next_signal() priority, also for asynchronously sent fault numbers.
 * Zero means no eligible signal. This does not distinguish queue ownership. */
int gw_signal_pending_next(uint64_t available);
void gw_signal_pending_discard(struct gw_signal_pending *, int);
void gw_signal_pending_close(struct gw_signal_pending *);
/* Only after senders, recipients and saved signal continuations quiesce. */
void gw_signal_pending_destroy(struct gw_signal_pending *);
#endif
