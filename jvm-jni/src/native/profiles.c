#include "adapter_internal.h"
#include "dlopen_backend.h"
#include "jre-adapter-profile.h"
#include <string.h>
/* The generated header must explicitly select a reviewed loader family and
 * libjava/libjvm digests. Unconfigured builds use an empty digest and refuse
 * install; obsolete version-specific build macros are intentionally
 * unsupported. */
#if !defined(GW_JVM_PROFILE_NAME) || !defined(GW_JVM_LIBJAVA_SHA256) || \
    !defined(GW_JVM_LIBJVM_SHA256) || !defined(GW_JVM_PROFILE_FAMILY)
#error \
    "Define GW_JVM_PROFILE_NAME, GW_JVM_LIBJAVA_SHA256, GW_JVM_LIBJVM_SHA256 and GW_JVM_PROFILE_FAMILY"
#endif
/* The singleton starts with the build-time profile; only
 * gw_jvm_jni_set_profile may replace it, and only while not installed. */
struct gw_jvm_adapter gw_jvm = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .profile = {.abi_version = GW_JVM_PROFILE_ABI_V2,
                .struct_size = sizeof(struct gw_jvm_profile_v2),
                .family = GW_JVM_PROFILE_FAMILY,
                .name = GW_JVM_PROFILE_NAME,
                .libjava_sha256 = GW_JVM_LIBJAVA_SHA256,
                .libjvm_sha256 = GW_JVM_LIBJVM_SHA256}};

/* Both exact artifact digests are mandatory. Reject old ABI/size before
 * accessing any fields added by v2. A digest is a review gate, not proof
 * that another build is incompatible. */
static int valid_digest(const char digest[65]) {
  if (digest[64]) {
    return 0;
  }
  for (unsigned i = 0; i < 64; i++) {
    if (!((digest[i] >= '0' && digest[i] <= '9') ||
          (digest[i] >= 'a' && digest[i] <= 'f'))) {
      return 0;
    }
  }
  return 1;
}

static int valid_profile(const struct gw_jvm_profile_v2 *p) {
  return p && p->abi_version == GW_JVM_PROFILE_ABI_V2 &&
         p->struct_size == sizeof *p && gw_jvm_shape(p->family) && p->name[0] &&
         memchr(p->name, 0, sizeof p->name) &&
         valid_digest(p->libjava_sha256) && valid_digest(p->libjvm_sha256);
}

/* Validates before locking, then stores a private copy under the adapter
 * lock; no caller pointer is retained. BUSY while installed: a hooked VM
 * never changes family or digest. */
int gw_jvm_jni_set_profile(const struct gw_jvm_profile_v2 *profile,
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
