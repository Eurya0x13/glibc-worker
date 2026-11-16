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

#ifdef __cplusplus
}
#endif
#endif
