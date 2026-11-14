#ifndef GW_JVM_ADAPTER_INTERNAL_H
#define GW_JVM_ADAPTER_INTERNAL_H
#include "jvm_adapter.h"
#include <pthread.h>

/* Static recipe for one hooked native: the declaring class, the name and
 * descriptor RegisterNatives must match, the libjava export holding the
 * original implementation, and the replacement proxy. */
struct gw_jvm_hook_shape {
  const char *owner, *name, *descriptor, *symbol;
  int is_static;
  void *proxy;
};

/* One loader family: the class whose fields the proxies publish into,
 * whether it has the JDK 8-9 `loaded` flag, and the hook slots in the fixed
 * order load, unload, find. */
struct gw_jvm_shape {
  const char *implementation;
  int has_loaded_field;
  struct gw_jvm_hook_shape hooks[3];
};

/* Resolved state for one slot: the pinned owner class, the replacement
 * entry used for registration and the original entry used for rollback. */
struct gw_jvm_hook {
  jclass owner;
  JNINativeMethod replacement, original;
};

/* Process-wide singleton. `lock` serializes profile updates against
 * install/restore; it cannot pause other VM threads. `installed` doubles as
 * the failed-rollback latch: it stays set so state the live proxies use is
 * never released. */
struct gw_jvm_adapter {
  pthread_mutex_t lock;
  int installed;
  struct gw_jvm_profile_v2 profile;
  const struct gw_jvm_shape *shape;
  const struct gw_loader_api_v1 *backend;
  void *libjava;
  jclass helper;
  /* Field ids on shape->implementation; loaded stays NULL without the flag. */
  jfieldID handle, version, from, loaded;
  /* Static helpers on dev.glibc.worker.JvmAdapter. */
  jmethodID verify, remember, forget, signature;
  struct gw_jvm_hook hooks[3];
};
extern struct gw_jvm_adapter gw_jvm;
/* NULL for an unknown family. */
const struct gw_jvm_shape *gw_jvm_shape(uint32_t family);
/* Raises UnsatisfiedLinkError unless an exception is already pending. */
void gw_jvm_throw(JNIEnv *, const struct gw_loader_error *);
/* GW_OK: published into the impl object. GW_PASS: not a Guest image, caller
 * uses the original path. GW_ERROR: failed; throw_if_fail only decides the
 * loader-not-found policy. */
int gw_jvm_open(JNIEnv *, jobject, jstring, int throw_if_fail);
/* GW_PASS when the handle is not backend-owned; errors throw. */
int gw_jvm_close(JNIEnv *, uint64_t);
/* Host trampoline address for a JNI export, or 0 when the lookup failed;
 * requires a live Guest handle. */
jlong gw_jvm_find(JNIEnv *, uint64_t, jstring);
#endif
