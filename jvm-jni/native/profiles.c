#include "adapter_internal.h"
#include "dlopen_backend.h"
#include "jre-adapter-profile.h"
#include <string.h>
/* Build-time default profile from the generated jre-adapter-profile.h. A
 * reviewed build defines GW_JVM_PROFILE_NAME, the libjava SHA256 and the
 * family itself; legacy embedders supply GW_JRE17_* and the family follows
 * GW_JRE17_LOAD_THREE_BOOL (two- vs three-boolean JRE17 load). An
 * unconfigured build keeps an empty digest and fails closed at install. */
#ifndef GW_JVM_PROFILE_NAME
#define GW_JVM_PROFILE_NAME GW_JRE17_PROFILE
#define GW_JVM_LIBJAVA_SHA256 GW_JRE17_LIBJAVA_SHA256
#ifndef GW_JRE17_LOAD_THREE_BOOL
#define GW_JRE17_LOAD_THREE_BOOL 0
#endif
#define GW_JVM_PROFILE_FAMILY                            \
  (GW_JRE17_LOAD_THREE_BOOL ? GW_JVM_NATIVE_LIBRARIES_18 \
                            : GW_JVM_NATIVE_LIBRARIES_15)
#endif

/* The singleton starts with the build-time profile; only
 * gw_jvm_jni_set_profile may replace it, and only while not installed. */
struct gw_jvm_adapter gw_jvm = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .profile = {.abi_version = GW_JVM_PROFILE_ABI_V1,
                .struct_size = sizeof(struct gw_jvm_profile_v1),
                .family = GW_JVM_PROFILE_FAMILY,
                .name = GW_JVM_PROFILE_NAME,
                .libjava_sha256 = GW_JVM_LIBJAVA_SHA256}};

/* A profile must be complete: exact v1 ABI and struct size, a family with a
 * known shape, a non-empty NUL-terminated name and exactly 64 lowercase hex
 * digits. The digest identifies the reviewed libjava artifact; it is a
 * review gate, not proof that another build is incompatible. */
static int valid_profile(const struct gw_jvm_profile_v1 *p) {
  if (!p || p->abi_version != GW_JVM_PROFILE_ABI_V1 ||
      p->struct_size != sizeof *p || !gw_jvm_shape(p->family) || !p->name[0] ||
      !memchr(p->name, 0, sizeof p->name) || p->libjava_sha256[64]) {
    return 0;
  }

  for (unsigned i = 0; i < 64; i++) {
    if (!((p->libjava_sha256[i] >= '0' && p->libjava_sha256[i] <= '9') ||
          (p->libjava_sha256[i] >= 'a' && p->libjava_sha256[i] <= 'f'))) {
      return 0;
    }
  }

  return 1;
}

/* Validates before locking, then stores a private copy under the adapter
 * lock; no caller pointer is retained. BUSY while installed: a hooked VM
 * never changes family or digest. */
int gw_jvm_jni_set_profile(const struct gw_jvm_profile_v1 *profile,
                           struct gw_loader_error *error) {
  if (!valid_profile(profile)) {
    return gw_dlopen_fail(error, GW_E_ARGUMENT,
                          "Invalid JVM family, profile ABI or SHA256");
  }

  pthread_mutex_lock(&gw_jvm.lock);
  int rc = GW_OK;
  if (gw_jvm.installed) {
    rc = gw_dlopen_fail(error, GW_E_BUSY, "JVM adapter is installed");
  } else {
    gw_jvm.profile = *profile;
  }

  pthread_mutex_unlock(&gw_jvm.lock);
  return rc;
}
