#ifndef GW_DOMAIN_SIGNAL_ROUTER_H
#define GW_DOMAIN_SIGNAL_ROUTER_H
#include <signal.h>
#include <stdint.h>

/* Host registration layer. Initialize before creating the VM or Guest owner.
 * This does not virtualize Guest libc's independently resolved sigaction. */
int signal_router_init(void);
int signal_router_enabled(void);
/* EIO pins a failed partial lifecycle until the owner disposes of the process.
 * Do not reclaim entries, retry admission, or automatically terminate Host. */
int signal_router_error(void);
int signal_router_quarantine(void);
typedef int (*signal_router_backend)(int, const struct sigaction *,
                                     struct sigaction *);
/* Explicit backend must be the bottom Host libc registration API. Late
 * integration owns registration quiescence and future import routing. */
int signal_router_init_with_backend(signal_router_backend);
int signal_router_adopt(void);
/* Only after a reviewed outer chain's bottom registration/query pointers
 * have been routed. NULL keeps the conservative standalone policy. */
int signal_router_adopt_chain(void *reviewed_chain_base);
/* Matching adoption libc ABI 7 uses these process-private kernel numbers.
 * Lease before starting its loader; EBUSY preserves any existing Host owner.
 * The lease is permanent: the GNU owner, timer helpers and images are pinned
 * until process exit, including after logical bridge/router shutdown. Host
 * registration and queries remain rejected after restore. Raw syscalls and
 * Host sends to leased channels are outside the signal ownership contract. */
#define GW_GUEST_NPTL_SIGNALS ((UINT64_C(1) << 62) | (UINT64_C(1) << 63))
int signal_router_lease_guest_nptl(void);
/* Keep GNU Host setxid handlers on their own TCB across adopted execution. */
int signal_router_route_host_setxid(void);
#define GW_GNU_HOST_CANCEL 32
#define GW_GNU_HOST_SETXID 33
#ifdef __ANDROID__
int signal_router_action64(int, const struct sigaction64 *,
                           struct sigaction64 *);
#endif
int signal_router_action(int, const struct sigaction *, struct sigaction *);
/* Integration-owned notification route, installed during serialized bootstrap
 * before admitting senders. The RT number must be unused and unreserved.
 * Entry has restored Host TLS and blocks every catchable signal. It runs before
 * logical Host selection, so an integration can classify the wake as hidden.
 * Targets must be registered Host carriers, never genuine GNU pthreads. The
 * callback and any saved frames remain pinned under the router's lifetime.
 * Normal callback return preserves Host errno.
 * This does not virtualize masks or establish a Guest handler/context ABI. */
int signal_router_internal_action(int, void (*)(int, siginfo_t *, void *));
int signal_router_dispatch_internal(int, siginfo_t *, void *, void *);
/* Enable independent synchronous Guest dispositions before any carrier runs.
 * Kernel routing remains installed even while Host selects SIG_DFL/SIG_IGN.
 * The caller serializes registration and retains this layer until shutdown. */
int signal_router_guest_faults(void);
/* Internal implementation of the public signal_channel.h bootstrap API.
 * Reserve six otherwise unused RT numbers before starting the VM. */
int signal_router_defer_faults(const int *notifications);
const int *signal_router_fault_notifications(void);
uint64_t signal_router_token_ignore_generation(void *);
int signal_router_token_restart(void *);
/* Select deferred logical delivery under the same writer transaction as Host
 * registrations. No application handler runs while the transaction is held.
 * Selection returns 0 (ignore), 1 (handler) or 2 (default). It consumes the
 * selected generation's one-shot before exposing a handler's logical mask. */
uint64_t signal_router_selection_begin(void);
void signal_router_selection_end(uint64_t);
int signal_router_select(int, const siginfo_t *, void *);
/* A successful SIG_IGN registration invalidates older logical pending work,
 * even if another registration replaces it before the recipient next runs. */
uint64_t signal_router_ignore_generation(int, int *ignored);
/* Join recipients and destroy the VM first. With a deferred channel, restore
 * runs on the enabling thread and restores its original private mask bits. */
/* Recoverable native write failures roll back all changed actions and permit
 * retry. Failed rollback returns EIO and pins the router in terminal failure.
 */
int signal_router_restore(void);
void signal_router_forward(int, siginfo_t *, void *, void *);
void signal_router_forward_domain(int, siginfo_t *, void *, void *, uint64_t,
                                  const stack_t *, uintptr_t, int);
void signal_router_forward_selected_domain(int, siginfo_t *, void *, void *,
                                           uint64_t, const stack_t *, uintptr_t,
                                           int, int);

/* The independent-entry Host frame may already occupy its logical signal
 * stack. Use the lowest retained SP on each stack. Optionally publish the
 * Guest stack floor before entering the Host handler. */
struct gw_host_stack_floors {
  uintptr_t ordinary, alternate;
};

void signal_router_forward_selected_suspended(
    int, siginfo_t *, void *, void *, uint64_t, const stack_t *, uintptr_t,
    const struct gw_host_stack_floors *, int, uintptr_t *);
_Noreturn void signal_router_default(int);
int signal_router_verify(void);
/* Admission guard: every pre-existing custom Host action must have been
 * explicitly adopted before any recipient can execute Guest code. */
int signal_router_guest_ready(void);
unsigned signal_router_registrations(void);
unsigned signal_router_retained(void);

/* Optional, explicit reclamation boundary; never inferred from reader counts.
 * enter must freeze new recipients/registrations and establish that every
 * possible recipient has left all routing entries, handlers and saved signal
 * frames. It must also prevent later setcontext/sigreturn into such a frame.
 * The calling thread must not itself be in a handler. enter runs with the
 * router writer lock held and catchable signals blocked. It may fail without
 * changing the router; on success leave releases the gate without waiting on
 * code needing the router lock. Neither callback may call this router.
 * A general JVM safepoint alone does not establish this signal quiescence.
 * For fully masked records, the built-in live proof is available separately
 * through signal_channel.h. */
struct signal_router_quiescence {
  uint32_t version, size;
  void *context;
  int (*enter)(void *);
  void (*leave)(void *);
};

#define GW_SIGNAL_ROUTER_QUIESCENCE_V1 1u
int signal_router_reclaim(const struct signal_router_quiescence *,
                          unsigned *reclaimed);
int signal_router_reclaim_signal(void);
uint64_t signal_router_reserved(void);
#endif
