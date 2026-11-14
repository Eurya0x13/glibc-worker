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
/* Supplies the selected real Host VM environment, never another VM's env. The
   owner must bootstrap the matching Guest and wait for its RPC loop before
   ready(). One runtime/VM per process; shutdown refuses live modules. */
#ifdef __cplusplus
extern "C" {
#endif
int full_host_start(struct shared *, JNIEnv *,
                    const struct gw_host_bridge_config *);
int full_host_ready(void);
int full_host_shutdown(void);
#ifdef __cplusplus
}
#endif
#endif
