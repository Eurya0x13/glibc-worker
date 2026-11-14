#ifndef GW_LOADER_BACKEND_H
#define GW_LOADER_BACKEND_H
#include <jni.h>
#include <stddef.h>
#include <stdint.h>

#include "loader_types.h"

#define GW_LOADER_ABI_V1 1u

struct gw_loader_library {
  uint64_t handle;
  jint jni_version;
};

/* Current Host thread must be attached to the selected Host VM. Paths,
   symbols and descriptors are borrowed for the call. Handles never are dlopen
   pointers. find() returns a Host trampoline valid until close(); its lifetime
   must be synchronized by the VM exactly as for ordinary native entry points.
   Pending Java exceptions survive failures; callers must check them. */
struct gw_loader_api_v1 {
  uint32_t abi_version;
  uint32_t struct_size;
  int (*open)(JNIEnv *, const char *requested_path, uint32_t flags,
              struct gw_loader_library *, struct gw_loader_error *);
  int (*find)(JNIEnv *, uint64_t handle, const char *symbol,
              const char *descriptor, void **host_entry,
              struct gw_loader_error *);
  int (*close)(JNIEnv *, uint64_t handle, struct gw_loader_error *);
  int (*owns_handle)(uint64_t handle); /* Recognizes retired Guest tags too. */
  int (*module_state)(uint64_t handle);
};

#ifdef __cplusplus
extern "C" {
#endif
__attribute__((visibility("default")))
const struct gw_loader_api_v1 *gw_loader_query(uint32_t abi_version,
                                               size_t minimum_size);
#ifdef __cplusplus
}
#endif
#endif
