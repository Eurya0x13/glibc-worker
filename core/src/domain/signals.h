#ifndef GW_DOMAIN_SIGNALS_INTERNAL_H
#define GW_DOMAIN_SIGNALS_INTERNAL_H
#include "domain.h"
#include "signal-deferred.h"
#include <stdatomic.h>
#include <sys/syscall.h>
#include <unistd.h>

enum { DOMAIN_FIXTURE_CARRIERS = 8 };

#include <signal.h>

/* Published before Guest entry, kept until the signal sender has stopped.
 * Fixed fixture capacity, not a proposed production registry design. */
struct signal_slot {
  uintptr_t host_tp, host_x18;
  struct exchange *exchange;
  _Atomic long tid;
  _Atomic int signals, faults, failures;
  void *signal_stack;
  stack_t old_stack;
  sigset_t inherited_mask;
  int inherited_blocked;
  struct gw_signal_deferred *deferred;
  uint64_t deferred_ignore_generation[GW_SIGNAL_FAULT_LANES];
  int deferred_active;
  int consume_fd;
};

_Static_assert(offsetof(struct signal_slot, tid) == 24, "signal assembly ABI");

extern struct signal_slot signal_slots[DOMAIN_FIXTURE_CARRIERS];
void signal_install(void);
void signal_register(struct signal_slot *, struct exchange *);
void signal_unregister(struct signal_slot *);
void signal_send(void);
void signal_note_guest_thread(int, long, uintptr_t);
struct signal_slot *signal_acquire(struct exchange *);
/* Registered Host carrier, all catchable signals blocked. A non-NULL frame
 * is the actual interrupted Host kernel context; NULL means Guest-origin
 * interruption and retains any outer Host anchor. Normally returning only. */
int signal_guest_enter(const struct domain_signal_call *, const stack_t *,
                       void *host_frame);
void signal_release(struct signal_slot *);
/* A live thread can retry EBUSY after unblocking/consuming its pending signals.
 * An exiting recipient discards its own notifications, like kernel pending
 * signals at native thread exit. Never drain another domain's signal numbers.
 */
int signal_prepare_release(struct signal_slot *, int thread_exiting);
int signal_host_retirement_busy(void);
unsigned signal_deferred_recipients(void);
void signal_guest_lifecycle(int);
int signal_guest_fault_bind(int (*)(int, void *, void *));
uint64_t signal_carrier_mask(struct signal_slot *, uint64_t, int guest);
uint64_t signal_host_physical_mask(uint64_t);
uint64_t signal_host_logical_mask(uint64_t);
uint64_t signal_host_pending(uint64_t);
uint64_t signal_host_wait_mask(uint64_t);
void signal_host_prune_notifications(void);
/* Host TLS and the current thread's registered recipient are required. */
int signal_host_defer(struct signal_slot *, const siginfo_t *);
int signal_host_take_notification(int, siginfo_t *);
/* -1 means no armed notification; zero means a consumed cancellation level. */
int signal_host_collect_notification(int, siginfo_t *);
/* Selection transaction already holds the router writer and blocks signals.
 * A private, nonblocking signalfd belongs to the recipient, never the caller.
 */
struct gw_signal_deferred *signal_host_consumer(void);
int signal_host_consumer_fd(void);
void domain_guest_thread_dispatch(int, siginfo_t *, void *, void *, uint64_t);
struct signal_slot *signal_dynamic_lookup(long, uintptr_t);
void domain_fault(void *);
extern char domain_fault_instruction[], domain_fault_resume[];
int adoption_signal_check(struct signal_slot *);

#endif
