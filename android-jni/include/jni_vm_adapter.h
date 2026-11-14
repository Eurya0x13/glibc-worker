#ifndef GW_JNI_VM_ADAPTER_H
#define GW_JNI_VM_ADAPTER_H
#include "loader_backend.h"

#define GW_JNI_VM_ADAPTER_V1 1u

/* Optional VM-specific integration. Register before full_host_start; the
 * table must remain alive until process exit. Public JNI needs no adapter. */
struct gw_jni_vm_adapter_v1 {
  uint32_t abi_version, struct_size;
  int (*install)(JNIEnv *, const char *native_directory,
                 const char *vm_library_path, struct gw_loader_error *);
  int (*restore)(JNIEnv *, struct gw_loader_error *);
};

#ifdef __cplusplus
extern "C" {
#endif
int gw_jni_register_vm_adapter(const struct gw_jni_vm_adapter_v1 *);
#ifdef __cplusplus
}
#endif
#endif
