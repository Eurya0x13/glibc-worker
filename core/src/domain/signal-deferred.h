#ifndef GW_SIGNAL_DEFERRED_H
#define GW_SIGNAL_DEFERRED_H
#include "signal-pending.h"
#include <sys/types.h>

enum { GW_SIGNAL_FAULT_LANES = 6 };

/* One recipient owns these lanes. Every operation runs on that recipient with
 * catchable signals blocked. The caller reserves six distinct RT signals for
 * the entire lifetime, including pending notifications and continuations.
 * Notifications carry no pointers: original siginfo lives in pending.
 */
struct gw_signal_deferred {
  struct gw_signal_pending pending;
  pid_t process, recipient;
  int notification[GW_SIGNAL_FAULT_LANES];
  uint64_t armed, promoted;
  int closed;
};

int gw_signal_deferred_init(struct gw_signal_deferred *, const int *);
/* A notification error leaves the original queued. Retry with arm(), not put().
 * Standard fault signals coalesce, retaining the first original siginfo.
 */
int gw_signal_deferred_put(struct gw_signal_deferred *, const siginfo_t *);
int gw_signal_deferred_arm(struct gw_signal_deferred *);
/* Call only after consuming a kernel notification. Zero means a tombstone. */
int gw_signal_deferred_take(struct gw_signal_deferred *, int, siginfo_t *);
/* For a notification already absent from kernel pending: -1 if unarmed. */
int gw_signal_deferred_collect(struct gw_signal_deferred *, int, siginfo_t *);
/* Host unblocking materializes original kernel numbers before delivery. Keep
 * shadow siginfo until selection, including a native same-number coalescence.
 * The caller holds the registration writer and blocks catchable signals. */
int gw_signal_deferred_promote(struct gw_signal_deferred *, uint64_t eligible);
int gw_signal_deferred_take_original(struct gw_signal_deferred *, int,
                                     siginfo_t *);
int gw_signal_deferred_level(const struct gw_signal_deferred *, int lane);
uint64_t gw_signal_deferred_levels(const struct gw_signal_deferred *);
void gw_signal_deferred_discard(struct gw_signal_deferred *, int);
/* Host notification masks follow the corresponding original logical mask.
 * Guest execution blocks all notifications; original fault bits are unchanged.
 */
uint64_t gw_signal_deferred_mask(const struct gw_signal_deferred *, uint64_t,
                                 int guest);
uint64_t gw_signal_deferred_visible(const struct gw_signal_deferred *,
                                    uint64_t);
uint64_t gw_signal_deferred_notifications(const struct gw_signal_deferred *);
uint64_t gw_signal_fault_bits(void);
uint64_t gw_signal_notification_mask(const int *, uint64_t logical);
/* Drain only cancelled level notifications, never live logical signals. */
void gw_signal_deferred_prune(struct gw_signal_deferred *);
/* Revoke after draining all kernel notifications, while signals stay blocked.
 */
int gw_signal_deferred_close(struct gw_signal_deferred *);
#endif
