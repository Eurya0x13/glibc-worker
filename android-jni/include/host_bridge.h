#ifndef GW_HOST_BRIDGE_H
#define GW_HOST_BRIDGE_H
#include "loader_backend.h"
struct shared;
#define GW_HOST_BRIDGE_CONFIG_V1 1u
#define GW_HOST_BRIDGE_CONFIG_V2 2u

enum gw_host_bridge_flags {
  GW_HOST_VM_ADAPTER = 1,
  GW_HOST_LEGACY_JRE17 = GW_HOST_VM_ADAPTER, /* Compatibility spelling. */
  GW_HOST_DIRECT_IMAGES = 2,
  /* Retained for callers that explicitly enabled the former opt-in mode. */
  GW_HOST_ARRAY_COPY_CACHE = 4,
  /* Automatic size selection is the default. This flag opts out and takes
     precedence over GW_HOST_ARRAY_COPY_CACHE. */
  GW_HOST_DISABLE_ARRAY_COPY_CACHE = 8
};

struct gw_host_bridge_config {
  uint32_t abi_version;
  uint32_t struct_size;
  uint32_t flags;
  const char *native_directory;
  const char *image_catalog;
  /* V2: actual libjava of the already running HotSpot. V1 uses
   * native_directory. */
  const char *jre_library_path;
};

#define GW_HOST_BRIDGE_CONFIG_V1_SIZE \
  offsetof(struct gw_host_bridge_config, jre_library_path)

/* Lifecycle of the single bridge bound to one selected target VM. ART and an
   embedded JRE are distinct VMs; only the selected VM's JNIEnv may enter here.
   STOPPED and QUARANTINED are terminal for this process. */
enum gw_host_state {
  GW_HOST_NEW,
  GW_HOST_STARTING,
  GW_HOST_STARTED, /* Host initialized; Guest handshake still required. */
  GW_HOST_READY,
  GW_HOST_STOPPING,
  GW_HOST_STOPPED,
  GW_HOST_QUARANTINED
};

/* Supplies the selected real Host VM environment, never another VM's env.
   Register adapters before start; serialize lifecycle operations on the start
   thread. After start succeeds, bootstrap the matching Guest and wait for its
   RPC loop before ready(). A failure after initialization begins is terminal.
   Before shutdown, the owner must stop admitting calls, stop/join mod work,
   detach Guest attachments and quiesce Host bindings, then close all modules.
   Idle Host bindings may remain until process exit. Shutdown refuses a busy
   bridge; it does not cancel calls, impose a deadline, destroy the target VM,
   or reclaim the GNU runtime. See docs/LIFECYCLE.md. */
#ifdef __cplusplus
extern "C" {
#endif
int full_host_start(struct shared *, JNIEnv *,
                    const struct gw_host_bridge_config *);
int full_host_ready(void);
int full_host_shutdown(void);
/* Any-thread atomic diagnostic snapshot; does not pin resources or prove that
   shutdown/cancellation is safe. No API here kills the process. */
enum gw_host_state full_host_state(void);
#ifdef __cplusplus
}
#endif
#endif
