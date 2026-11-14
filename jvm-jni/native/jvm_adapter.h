#ifndef GW_JVM_ADAPTER_H
#define GW_JVM_ADAPTER_H
#include "loader_backend.h"
#define GW_JVM_PROFILE_ABI_V1 1u

/* These identify contracts, including parameter semantics, not JVM majors. */
enum gw_jvm_loader_family {
  GW_JVM_CLASSLOADER_8 = 1,
  GW_JVM_CLASSLOADER_10,
  GW_JVM_CLASSLOADER_10_THROW,
  GW_JVM_NATIVE_LIBRARIES_15,
  GW_JVM_NATIVE_LIBRARIES_18,
  GW_JVM_NATIVE_LIBRARIES_19
};

struct gw_jvm_profile_v1 {
  uint32_t abi_version, struct_size, family;
  char name[128];
  char libjava_sha256[65];
};

#ifdef __cplusplus
extern "C" {
#endif
/* Copies a reviewed profile. Call while the adapter is stopped and quiescent.
 * Matching hash and complete method/field checks precede any registration. */
int gw_jvm_jni_set_profile(const struct gw_jvm_profile_v1 *,
                           struct gw_loader_error *);
int gw_jvm_adapter_install(JNIEnv *, const char *native_directory,
                           const char *libjava_path, struct gw_loader_error *);
int gw_jvm_adapter_restore(JNIEnv *, struct gw_loader_error *);
int gw_jvm_jni_register(void);
#ifdef __cplusplus
}
#endif
#endif
