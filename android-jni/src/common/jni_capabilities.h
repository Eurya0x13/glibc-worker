#ifndef GW_JNI_CAPABILITIES_H
#define GW_JNI_CAPABILITIES_H
#include <jni.h>

/* JNI 9 adds GetModule; JNI 10 adds a version constant but no table slot.
 * Newer VM tables are not exposed until their additional slots are bridged. */
#define GW_JNI_MAX_VERSION 0x000a0000

static inline int gw_jni_known_version(jint version) {
  return version == JNI_VERSION_1_1 || version == JNI_VERSION_1_2 ||
         version == JNI_VERSION_1_4 || version == JNI_VERSION_1_6 ||
         version == 0x00010008 || version == 0x00090000 ||
         version == GW_JNI_MAX_VERSION;
}

static inline jint gw_jni_negotiate(jint host_version) {
  if (host_version > GW_JNI_MAX_VERSION) {
    return GW_JNI_MAX_VERSION;
  }
  return gw_jni_known_version(host_version) ? host_version : 0;
}

static inline int gw_jni_accepts(jint capability, jint requested) {
  return capability && requested <= capability &&
         gw_jni_known_version(requested);
}
#endif
