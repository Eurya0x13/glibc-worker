#ifndef GW_JVM_ADAPTER_H
#define GW_JVM_ADAPTER_H
#include "loader_backend.h"
#define GW_JVM_PROFILE_ABI_V2 2u

/* These identify contracts, including parameter semantics, not JVM majors. */
enum gw_jvm_loader_family {
  GW_JVM_CLASSLOADER_8 = 1,
  GW_JVM_CLASSLOADER_10,
  GW_JVM_CLASSLOADER_10_THROW,
  GW_JVM_NATIVE_LIBRARIES_15,
  GW_JVM_NATIVE_LIBRARIES_18,
  GW_JVM_NATIVE_LIBRARIES_19
};

/* The reviewed build this adapter accepts: family selects the hook recipe,
 * and the two lowercase-hex SHA-256 digests pin libjava.so and libjvm.so.
 * Both must match before any native method is replaced. */
struct gw_jvm_profile_v2 {
  uint32_t abi_version, struct_size, family;
  char name[128];
  char libjava_sha256[65];
  char libjvm_sha256[65];
};

#ifdef __cplusplus
extern "C" {
#endif
/* Copies a reviewed profile. Call while the adapter is stopped and quiescent.
 * Matching hash and complete method/field checks precede any registration. */
int gw_jvm_jni_set_profile(const struct gw_jvm_profile_v2 *,
                           struct gw_loader_error *);
/* Installs the proxies into the VM env belongs to: native_directory names a
 * JRE layout (libjava.so appended) and libjava_path overrides it. The
 * artifact must already be resident and match the profile digests; failure
 * leaves the VM untouched or fully restored. */
int gw_jvm_adapter_install(JNIEnv *, const char *native_directory,
                           const char *libjava_path, struct gw_loader_error *);
/* Re-registers libjava's original natives. Full success releases every
 * reference and makes the adapter reusable; failure keeps state for retry. */
int gw_jvm_adapter_restore(JNIEnv *, struct gw_loader_error *);
/* Publishes the adapter table; call before the bridge starts, as the set is
 * frozen afterwards. A constructor already runs this at DSO load. */
int gw_jvm_jni_register(void);
#ifdef __cplusplus
}
#endif
#endif
