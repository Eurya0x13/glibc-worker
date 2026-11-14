#include "rpc_metrics_internal.h"

/* A separate translation unit must use the same TLS row as its caller. */
void rpc_metrics_peer_run(void) {
#if GW_RPC_PROFILE
  struct gw_rpc_token nested = gw_rpc_begin();
  gw_rpc_end(nested);
#endif
}
