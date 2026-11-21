#ifndef GW_HOST_SIGNAL_BOOTSTRAP_H
#define GW_HOST_SIGNAL_BOOTSTRAP_H

#include "signal_contract.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Link glibc_worker::signal_bootstrap into the native Host executable. Call
 * from its serialized startup before loading a VM, starting other threads or
 * admitting Guest code. Inherited Host actions are adopted and checked.
 * Returns zero or an errno value. EALREADY means the router is active;
 * ENOTSUP includes an already loaded ART/HotSpot or missing interposition.
 * EIO means the caller must retain mappings and dispose of the process. */
int gw_host_signal_bootstrap(void);

/* Capture the current ownership contract for runtime start. Call after
 * configuring any optional Host signal channels. On failure, the caller's
 * output remains intact. Returns zero or an errno value. */
int gw_host_signal_contract_init(struct gw_signal_contract *);

/* Restore the inherited Host actions after runtime shutdown, VM destruction
 * and recipient quiescence. Call on the bootstrap thread. EBUSY can be
 * retried after the remaining owners quiesce. EIO is terminal. The launcher
 * still owns process disposal and keeps the router image mapped. */
int gw_host_signal_restore(void);

#ifdef __cplusplus
}
#endif
#endif
