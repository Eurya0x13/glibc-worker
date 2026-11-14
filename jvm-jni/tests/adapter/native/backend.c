#define _GNU_SOURCE
#include "jvm_adapter.h"
#include "jni_vm_adapter.h"
#include "dlopen_backend.h"
#include <dlfcn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* This transport executes on the real desktop VM thread. It tests the JVM
 * adapter contract, not Android or two-libc execution. */
static void *images[FB_MODULES];
static _Thread_local uint64_t current;
static const struct gw_jni_vm_adapter_v1 *adapter;
static _Atomic int loads, closes, rejected, lookups;
static const struct gw_dlopen_api_v1 *base;
static unsigned fail_registrations, registration_count;
static int wrong_vm;
static JavaVM shadow_vm;
static jint (*register_original)(JNIEnv *, jclass, const JNINativeMethod *,
                                 jint);
static char last_error[GW_LOADER_DIAGNOSTIC];

static jint foreign_vm(JNIEnv *env, JavaVM **vm) {
  (void)env;
  *vm = &shadow_vm;
  return JNI_OK;
}

static jint register_fault(JNIEnv *env, jclass owner,
                           const JNINativeMethod *methods, jint count) {
  unsigned call = registration_count++;
  if (call < 32 && (fail_registrations & (1u << call))) {
    return JNI_ERR;
  }
  return register_original(env, owner, methods, count);
}

/* Test-only injection on this VM thread. Keep the real JNIEnv address and all
 * CheckJNI functions; replace only RegisterNatives until install returns. */
static int install_with_fault(JNIEnv *env, const char *file,
                              struct gw_loader_error *error) {
  if (!fail_registrations && !wrong_vm) {
    return adapter->install(env, NULL, file, error);
  }

  const struct JNINativeInterface_ *original = *env;
  jint version = (*env)->GetVersion(env);
  if (version >= 0x001c0000) {
    return gw_dlopen_fail(error, GW_E_ARGUMENT,
                          "Fault injection supports JNI up to 25");
  }

  /* Preserve the actual VM table, not sizeof the compile JDK's table. JNI9
   * adds GetModule, 19 IsVirtual, and 24 GetStringUTFLengthAsLong. Copying a
   * JDK17-sized structure would read past JRE8's table. */
  size_t slots = version >= 0x00180000   ? 236
                 : version >= 0x00130000 ? 235
                 : version >= 0x00090000 ? 234
                                         : 233;

  struct JNINativeInterface_ *table = malloc(slots * sizeof(void *));
  if (!table) {
    return gw_dlopen_fail(error, GW_E_BRIDGE,
                          "Cannot allocate fault injection table");
  }

  memcpy(table, original, slots * sizeof(void *));
  register_original = original->RegisterNatives;
  table->RegisterNatives = register_fault;
  if (wrong_vm) {
    JavaVM *actual = NULL;
    if (original->GetJavaVM(env, &actual) != JNI_OK || !actual) {
      free(table);
      return GW_ERROR;
    }
    shadow_vm = *actual;
    table->GetJavaVM = foreign_vm;
  }
  registration_count = 0;
  *env = table;

  int rc = adapter->install(env, NULL, file, error);

  *env = original;
  free(table);
  fail_registrations = 0;
  return rc;
}

int gw_jni_register_vm_adapter(const struct gw_jni_vm_adapter_v1 *value) {
  adapter = value;
  return GW_OK;
}

static int load_image(void *context, uint64_t id, const char *path,
                      int32_t *version, struct gw_loader_error *error) {
  JNIEnv *env = context;
  int slot = fb_module_index(id);
  images[slot] = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (!images[slot]) {
    return gw_dlopen_fail(error, GW_E_LOAD, dlerror());
  }

  JavaVM *vm = NULL;
  if ((*env)->GetJavaVM(env, &vm) != JNI_OK) {
    dlclose(images[slot]);
    images[slot] = NULL;
    return gw_dlopen_fail(error, GW_E_BRIDGE, "Cannot obtain test JavaVM");
  }

  jint (*onload)(JavaVM *, void *) = (void *)dlsym(images[slot], "JNI_OnLoad");
  uint64_t saved = current;
  current = id;
  *version = onload ? onload(vm, NULL) : JNI_VERSION_1_1;
  current = saved;

  if (*version != JNI_VERSION_1_8 || (*env)->ExceptionCheck(env)) {
    dlclose(images[slot]);
    images[slot] = NULL;
    atomic_fetch_add(&rejected, 1);
    return gw_dlopen_fail(error, GW_E_VERSION, "Fixture JNI_OnLoad rejected");
  }

  atomic_fetch_add(&loads, 1);
  return GW_OK;
}

static int close_image(void *context, uint64_t id,
                       struct gw_loader_error *error) {
  (void)error;
  JNIEnv *env = context;
  int slot = fb_module_index(id);
  JavaVM *vm = NULL;
  if ((*env)->GetJavaVM(env, &vm) != JNI_OK) {
    return GW_ERROR;
  }

  void (*unload)(JavaVM *, void *) =
      (void *)dlsym(images[slot], "JNI_OnUnload");
  if (unload) {
    unload(vm, NULL);
  }

  dlclose(images[slot]);
  images[slot] = NULL;
  atomic_fetch_add(&closes, 1);
  return GW_OK;
}

static int find_image(void *context, uint64_t id, const char *symbol,
                      const char *signature, void **entry,
                      struct gw_loader_error *error) {
  (void)context;
  if (strcmp(signature, "(J)J") && strcmp(signature, "(I)I") &&
      strcmp(signature, "(D)D")) {
    return gw_dlopen_fail(error, GW_E_ARGUMENT,
                          "Incorrect reflected descriptor");
  }
  *entry = dlsym(images[fb_module_index(id)], symbol);
  atomic_fetch_add(&lookups, 1);
  return *entry ? GW_OK : GW_NOT_FOUND;
}

static uint64_t current_image(void *context) {
  (void)context;
  return current;
}

static int resolve_image(void *context, const char *path,
                         char image[GW_LOADER_PATH],
                         struct gw_loader_error *error, void *opaque) {
  (void)context;
  (void)error;
  (void)opaque;
  snprintf(image, GW_LOADER_PATH, "%s", path);
  return GW_OK;
}

static int open_library(JNIEnv *env, const char *path, uint32_t flags,
                        struct gw_loader_library *library,
                        struct gw_loader_error *error) {
  (void)flags;
  if (!strstr(path, "libadapter_fixture")) {
    return GW_PASS;
  }
  struct gw_dlopen_library result = {0};
  int rc = base->open(env, path, GW_OPEN_GUEST, &result, error);
  library->handle = result.handle;
  library->jni_version = result.metadata;
  return rc;
}

static int find_library(JNIEnv *env, uint64_t id, const char *symbol,
                        const char *signature, void **entry,
                        struct gw_loader_error *error) {
  return base->find(env, id, symbol, signature, entry, error);
}

static int close_library(JNIEnv *env, uint64_t id,
                         struct gw_loader_error *error) {
  return base->close(env, id, error);
}

static int owns(uint64_t id) {
  return base->owns_handle(id);
}

static int state(uint64_t id) {
  return base->module_state(id);
}
static const struct gw_loader_api_v1 api = {
    GW_LOADER_ABI_V1, sizeof api, open_library, find_library,
    close_library,    owns,       state};

const struct gw_loader_api_v1 *gw_loader_query(uint32_t version, size_t size) {
  return version == GW_LOADER_ABI_V1 && size <= sizeof api ? &api : NULL;
}

JNIEXPORT jint JNICALL Java_AdapterTest_installProfile(JNIEnv *env, jclass cls,
                                                       jstring path,
                                                       jstring sha,
                                                       jstring vmsha,
                                                       jint family) {
  (void)cls;
  struct gw_jvm_profile_v2 profile = {GW_JVM_PROFILE_ABI_V2,
                                      sizeof profile,
                                      (uint32_t)family,
                                      "desktop-contract",
                                      "",
                                      ""};
  const char *digest = (*env)->GetStringUTFChars(env, sha, NULL);
  if (!digest) {
    return GW_ERROR;
  }
  snprintf(profile.libjava_sha256, sizeof profile.libjava_sha256, "%s", digest);
  (*env)->ReleaseStringUTFChars(env, sha, digest);

  digest = (*env)->GetStringUTFChars(env, vmsha, NULL);
  if (!digest) {
    return GW_ERROR;
  }
  snprintf(profile.libjvm_sha256, sizeof profile.libjvm_sha256, "%s", digest);
  (*env)->ReleaseStringUTFChars(env, vmsha, digest);
  struct gw_loader_error error = {0};
  int rc = gw_jvm_jni_set_profile(&profile, &error);
  if (rc != GW_OK) {
    snprintf(last_error, sizeof last_error, "%s", error.message);
    return rc;
  }

  const char *file = (*env)->GetStringUTFChars(env, path, NULL);
  if (!file) {
    return GW_ERROR;
  }

  rc = install_with_fault(env, file, &error);
  snprintf(last_error, sizeof last_error, "%s", error.message);
  (*env)->ReleaseStringUTFChars(env, path, file);
  return rc;
}

/* The v1-sized object is deliberately shorter than v2. Rejection must
 * happen on ABI/size before any access to the added digest. */
JNIEXPORT jint JNICALL Java_AdapterTest_oldProfile(JNIEnv *env, jclass cls) {
  (void)env;
  (void)cls;

  struct {
    uint32_t abi_version, struct_size, family;
    char name[128], libjava_sha256[65];
  } old = {.abi_version = 1,
           .struct_size = sizeof old,
           .family = GW_JVM_NATIVE_LIBRARIES_19,
           .name = "obsolete"};
  struct gw_loader_error error = {0};
  return gw_jvm_jni_set_profile((const void *)&old, &error);
}

JNIEXPORT jint JNICALL Java_AdapterTest_restore(JNIEnv *env, jclass cls) {
  (void)cls;
  struct gw_loader_error error = {0};
  return adapter->restore(env, &error);
}

JNIEXPORT jint JNICALL
Java_AdapterTest_vmMismatchProfile(JNIEnv *env, jclass cls, jstring path,
                                   jstring sha, jstring vmsha, jint family) {
  wrong_vm = 1;
  int result =
      Java_AdapterTest_installProfile(env, cls, path, sha, vmsha, family);
  wrong_vm = 0;
  return result;
}

JNIEXPORT void JNICALL Java_AdapterTest_faults(JNIEnv *env, jclass cls,
                                               jint mask) {
  (void)env;
  (void)cls;
  fail_registrations = (unsigned)mask;
}

JNIEXPORT jstring JNICALL Java_AdapterTest_error(JNIEnv *env, jclass cls) {
  (void)cls;
  return (*env)->NewStringUTF(env, last_error);
}

JNIEXPORT jintArray JNICALL Java_AdapterTest_metrics(JNIEnv *env, jclass cls) {
  (void)cls;
  jint values[] = {loads, closes, rejected, lookups};
  jintArray out = (*env)->NewIntArray(env, 4);
  if (out) {
    (*env)->SetIntArrayRegion(env, out, 0, 4, values);
  }
  return out;
}

JNIEXPORT jint JNICALL Java_AdapterTest_stop(JNIEnv *env, jclass cls) {
  (void)env;
  (void)cls;
  struct gw_loader_error error = {0};
  return gw_dlopen_stop(&error);
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *unused) {
  (void)vm;
  (void)unused;
  struct gw_dlopen_config config = {
      .transport = {load_image, close_image, find_image, current_image},
      .resolve_image = resolve_image};
  struct gw_loader_error error = {0};

  base = gw_dlopen_query(GW_DLOPEN_ABI_V1, sizeof *base);

  return base && gw_dlopen_configure(&config, &error) == GW_OK &&
                 gw_dlopen_activate(&error) == GW_OK
             ? JNI_VERSION_1_8
             : JNI_ERR;
}
