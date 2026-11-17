#ifndef GW_DOMAIN_SIGNALS_INTERNAL_H
#define GW_DOMAIN_SIGNALS_INTERNAL_H
#include "domain.h"
#include "signal-deferred.h"
#include <stdatomic.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <ucontext.h>

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
  int guest_async;
};

_Static_assert(offsetof(struct signal_slot, tid) == 24, "signal assembly ABI");

extern struct signal_slot signal_slots[DOMAIN_FIXTURE_CARRIERS];
void signal_install(void);
void signal_register(struct signal_slot *, struct exchange *);
void signal_unregister(struct signal_slot *);
void signal_send(void);
void signal_note_guest_thread(int, long, uintptr_t);
struct signal_slot *signal_acquire(struct exchange *);
void signal_release(struct signal_slot *);
/* A live thread can retry EBUSY after unblocking/consuming its pending signals.
 * An exiting recipient discards its own notifications, like kernel pending
 * signals at native thread exit. Never drain another domain's signal numbers.
 */
int signal_prepare_release(struct signal_slot *, int thread_exiting);
int signal_host_retirement_busy(void);
unsigned signal_deferred_recipients(void);
void signal_guest_lifecycle(int);
/* GNU pthread signals tail-enter this Guest-owned kernel handler, with no
 * Host C/assembly frame left underneath native forced unwind. */
int signal_guest_kernel_bind(void (*)(int, void *, void *));
/* Reserve an internal notifier for native GNU recipients. Install before
 * their loader starts. The number stays leased until router shutdown. */
int signal_guest_kernel_notify_enable(int, void (*)(int, siginfo_t *, void *));
void signal_guest_notification_discard(void *);
/* Only the owning registered Host thread changes its lane. Detach after the
 * Guest registry has revoked sends and before unmasking or releasing TLS. */
int signal_guest_carrier_attach(struct signal_slot *);
int signal_guest_carrier_detach(struct signal_slot *);

/* A live kernel Guest frame below a routed Host handler. Valid until that
 * handler returns or retires through its native unwind boundary. */
struct gw_signal_suspension {
  ucontext_t *frame;
  struct domain state;
  stack_t alternate;
  uintptr_t stack_floor;
};

int signal_guest_suspended(struct signal_slot *, struct gw_signal_suspension *);
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
uintptr_t domain_guest_thread_select(int, const siginfo_t *);
struct signal_slot *signal_dynamic_lookup(long, uintptr_t);
void domain_fault(void *);
extern char domain_fault_instruction[], domain_fault_resume[];
int adoption_signal_check(struct signal_slot *);

#endif
