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
  uint64_t real_blocked;
  uint64_t deliveries, interrupted_io;
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
  _Atomic uint32_t wait_sequence;
  struct gw_signal_inbox inbox;
  struct gw_signal_target **slots, **active;
  size_t capacity, occupied, count, active_capacity, retained;
  gw_signal_target_notify notify;
  void *context;
  int closed, wake_error, waits_enabled;
};

void gw_signal_targets_init(struct gw_signal_targets *, uint64_t, uint64_t,
                            gw_signal_target_notify, void *);
int gw_signal_target_open(struct gw_signal_targets *, uintptr_t, int, uint64_t,
                          void *, struct gw_signal_target **);
/* Reserve identity/storage before clone. The returned creator pin keeps the
 * handle alive across child exit and descriptor reuse. A prepared target has
 * tid == 0: no entry, consumption or notification until activation. */
int gw_signal_target_prepare(struct gw_signal_targets *, uintptr_t, uint64_t,
                             void *, struct gw_signal_target **);
/* Allocation-free admission after successful native clone. Repeating the same
 * TID is harmless; a different TID returns EALREADY. Initial notification
 * failure stays in targets_error(), separate from committed admission. */
int gw_signal_target_activate(struct gw_signal_target *, int);
struct gw_signal_publication gw_signal_targets_send(struct gw_signal_targets *,
                                                    uintptr_t, int,
                                                    const siginfo_t *);
/* Disposition owner has already validated the packet. Ignored signals still
 * queue when the directed target (or explicit process representative) blocks
 * them, including a temporary wait's saved real mask. The representative must
 * remain pinned, even after native thread exit, until sends are revoked. */
struct gw_signal_publication gw_signal_targets_send_filtered(
    struct gw_signal_targets *, uintptr_t, int, const siginfo_t *, int ignored,
    struct gw_signal_target *representative);
struct gw_signal_publication gw_signal_target_mask(struct gw_signal_target *,
                                                   uint64_t);
struct gw_signal_publication gw_signal_target_masks(struct gw_signal_target *,
                                                    uint64_t, uint64_t);
int gw_signal_target_deliver(struct gw_signal_target *, siginfo_t *,
                             enum gw_signal_queue_source *);
/* A consuming wait may take logically blocked signals. Retain the same
 * thread-before-shared selection and registry transaction as handler entry. */
int gw_signal_target_take(struct gw_signal_target *, uint64_t eligible,
                          siginfo_t *, enum gw_signal_queue_source *);
/* Enable consuming waits before admission. The domain futex sequence changes
 * on accepted publication, recipient closure and actual handler entry.
 * Waiters own their recipient for the duration of the native call. */
int gw_signal_targets_enable_waits(struct gw_signal_targets *);

struct gw_signal_wait_sample {
  uint32_t sequence;
  uint64_t deliveries, interrupted_io;
};

int gw_signal_target_wait_take(struct gw_signal_target *, uint64_t, siginfo_t *,
                               struct gw_signal_wait_sample *);
void gw_signal_target_note_delivery(struct gw_signal_target *, int restart);
int gw_signal_targets_wake_waiters(struct gw_signal_targets *);
uint64_t gw_signal_target_blocked_pending(struct gw_signal_target *);
/* Flush all pending instances of one signal under the registry writer,
 * releasing aggregate quota. Disposition owners must also serialize the new
 * action and future sends; this operation does not install SIG_IGN itself. */
int gw_signal_targets_discard(struct gw_signal_targets *, int);
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
/* Drop one owner pin and release only if this is the last pin on a closed
 * recipient. This combines parent/child retirement in one writer transaction.
 * Close in this protocol requires established recipient/frame quiescence. */
int gw_signal_target_retire(struct gw_signal_target *);
int gw_signal_targets_retry(struct gw_signal_targets *);
int gw_signal_targets_error(struct gw_signal_targets *);
int gw_signal_targets_only(struct gw_signal_targets *,
                           struct gw_signal_target *);
void gw_signal_targets_close(struct gw_signal_targets *);
int gw_signal_targets_destroy(struct gw_signal_targets *);
#endif
