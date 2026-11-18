#ifndef GW_RUNTIME_BRIDGE_H
#define GW_RUNTIME_BRIDGE_H
#include "host_bridge.h"
#include "signal_contract.h"

#define GW_HOST_RUNTIME_CONFIG_V1 1u
#define GW_HOST_RUNTIME_CONFIG_V2 2u

enum gw_execution_backend {
  GW_EXECUTION_SAME_THREAD = 0,
  /* Compatibility backend in maintenance mode. */
  GW_EXECUTION_WORKER = 1
};

struct gw_host_runtime_config {
  uint32_t version;
  uint32_t size;
  enum gw_execution_backend backend;

  union {
    uint32_t reserved;           /* V1: required zero. */
    uint32_t guest_notification; /* V2: explicit unused kernel RT number. */
  };

  const char *loader_path;
  const char *library_path;
  const char *service_path;
  const struct gw_host_bridge_config *bridge;
  const struct gw_signal_contract *signals;
};

#ifdef __cplusplus
extern "C" {
#endif
/* High-level start for one runtime and one selected real VM per process.
 * SAME_THREAD is the zero/default backend. The caller prepares the Host signal
 * router before releasing VM/application threads; jvm-jni supplies any pinned
 * VM/ART integration. This API never selects a VM profile automatically.
 * Paths must refer to the matching packaged loader, GNU service and libc.
 * Returns zero or an errno value. Validation failures that leave the bridge
 * NEW admit a corrected retry without creating helpers. Once the bridge
 * claims initialization, failure is terminal: retain mappings and dispose of
 * the process, never retry start. An existing relay is borrowed; otherwise
 * startup creates and owns it before admitting GNU code.
 * The selected VM is obtained from env by the bridge, not created here.
 * Use a dedicated bootstrap/lifecycle thread. Its context zero is the GNU
 * supervisor's control channel; application JNI calls use other VM threads.
 * Overlapping lifecycle calls return EBUSY before accessing that channel. */
int gw_host_runtime_start(JNIEnv *env, const struct gw_host_runtime_config *);

/* Same lifecycle owner and quiescence requirements as full_host_shutdown.
 * Success does not unload the runtime, destroy the VM or reclaim idle domain
 * continuations. The bridge and signal router stay mapped until process exit.
 * EBUSY permits retry after producers quiesce and Guest descriptors close.
 * Once shutdown commits, a failed owner acknowledgement or dispatcher join
 * returns terminal EIO. A completed shutdown returns EALREADY on repetition. */
int gw_host_runtime_shutdown(void);
#ifdef __cplusplus
}
#endif
#endif
