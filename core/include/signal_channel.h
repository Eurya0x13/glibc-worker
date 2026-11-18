#ifndef GW_HOST_SIGNAL_CHANNEL_H
#define GW_HOST_SIGNAL_CHANNEL_H
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { GW_HOST_FAULT_SIGNAL_COUNT = 6 };

/* Link glibc_worker::signal_interpose and prepare/adopt the Host router first.
 * Before starting the VM, creating recipients or exposing Host signalfds,
 * reserve six distinct, unused public RT numbers. Supply the actual reviewed
 * numbers: this API does not select them or override existing registrations.
 * The channel preserves Host pending/consuming behavior while Guest execution
 * needs the six synchronous-fault numbers unblocked. It does not implement
 * independent Guest asynchronous queues or tagged sends.
 *
 * Returns zero or an errno value. EBUSY includes existing Host signalfds or
 * occupied registrations. EIO is terminal: retain code and dispose of the
 * process. The lifecycle owner joins recipients and closes/clears descriptors
 * containing the reserved notification numbers before restoring the router.
 * Runtime-owned descriptors must not be closed/replaced by application code.
 */
int gw_host_signal_channel_enable(const int *notifications, size_t count);

/* Optional live reclamation. Lease an unused public realtime number before
 * starting the runtime's relay/Guest helpers. The process owns this number
 * until exit, including across router restore. Keep the router image loaded.
 * The owner must exclude application use, signal consumption and raw changes
 * to the reserved action. */
int gw_host_signal_reclaim_enable(int number);

/* Call on a normal Host control thread, outside signal handlers. Reclaim
 * replaced generations that have a fully masked kernel entry (including
 * deferred fault channels), plus unpublished data-only generations. Others
 * still require the embedding's complete recipient/continuation barrier.
 * Missing native acknowledgements or a changing task census return EBUSY.
 * Probe delivery uses SA_RESTART and the installed Host wait adapters hide
 * it. Non-restartable waits outside those adapters may receive EINTR.
 * Returns an errno value. Failure leaves the output and generations intact. */
int gw_host_signal_reclaim(unsigned timeout_ms, unsigned *reclaimed);

#ifdef __cplusplus
}
#endif
#endif
