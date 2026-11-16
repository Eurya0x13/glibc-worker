#ifndef GW_DOMAIN_SIGNAL_RELAY_H
#define GW_DOMAIN_SIGNAL_RELAY_H
#include <signal.h>
#include <stdint.h>

/* Explicit change of delivery thread, never Host TLS impersonation. Start on
 * a Host lifecycle thread before admitting GNU threads. The dispatcher is a
 * real Host pthread, not an implicitly attached JVM thread. A Guest signal
 * entry waits for its request to return, keeping the original siginfo and
 * kernel context alive. Handlers run serially with Host TLS; their ucontext
 * describes the original GNU recipient. Cross-OS-thread nonlocal jumps and
 * synchronously waiting for that recipient from its handler are invalid.
 * Stop only after external senders and GNU recipients have quiesced/joined.
 * Pending work returns EBUSY; restore/reclaim must not outlive queued tokens.
 */
int signal_host_relay_start(void);
int signal_host_relay_stop(void);
int signal_host_relay_running(void);
int signal_host_relay_busy(void);

/* Called with all catchable signals blocked, possibly under GNU TLS. Uses
 * only lock-free atomics and raw syscalls, never Host libc/TLS/allocation. */
int signal_host_relay(int, siginfo_t *, void *, void *, uint64_t);
#endif
