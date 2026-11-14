#include "adapter_internal.h"
#include <stdio.h>

/* Turns a loader error into UnsatisfiedLinkError, the exception HotSpot
 * raises from these natives; a pending exception always wins. */
void gw_jvm_throw(JNIEnv *env, const struct gw_loader_error *error) {
  if ((*env)->ExceptionCheck(env)) {
    return;
  }

  jclass cls = (*env)->FindClass(env, "java/lang/UnsatisfiedLinkError");
  if (cls) {
    (*env)->ThrowNew(
        env, cls,
        error->message[0] ? error->message : "Guest operation failed");
    (*env)->DeleteLocalRef(env, cls);
  }
}

/* Undoes a backend open whose Java-side publish failed: forget the owner,
 * close the Guest module, then re-raise the original exception; secondary
 * failures are cleared for it. A failed close leaks the handle and is only
 * reported for process disposal. */
static void rollback_open(JNIEnv *env, uint64_t id) {
  jthrowable cause = (*env)->ExceptionOccurred(env);
  if (cause) {
    (*env)->ExceptionClear(env);
  }

  (*env)->CallStaticVoidMethod(env, gw_jvm.helper, gw_jvm.forget, (jlong)id);
  if ((*env)->ExceptionCheck(env)) {
    (*env)->ExceptionClear(env);
  }

  struct gw_loader_error error = {0};
  if (gw_jvm.backend->close(env, id, &error) != GW_OK) {
    fprintf(stderr,
            "JVM_JNI rollback_close_failed handle=%llx "
            "process_disposal_required=1\n",
            (unsigned long long)id);
  }

  if (cause) {
    if ((*env)->ExceptionCheck(env)) {
      (*env)->ExceptionClear(env);
    }
    (*env)->Throw(env, cause);
    (*env)->DeleteLocalRef(env, cause);
  }
}

/* Commits an accepted load into the impl object. The owner class is
 * remembered first (signature lookups need it), then handle, jniVersion
 * and, in the 8-9 layout, loaded are written. Any JNI failure returns 0
 * and the caller rolls the whole load back. */
static int publish_library(JNIEnv *env, jobject impl,
                           const struct gw_loader_library *library) {
  jobject from = (*env)->GetObjectField(env, impl, gw_jvm.from);
  if ((*env)->ExceptionCheck(env)) {
    return 0;
  }

  (*env)->CallStaticVoidMethod(env, gw_jvm.helper, gw_jvm.remember,
                               (jlong)library->handle, from);
  if (from) {
    (*env)->DeleteLocalRef(env, from);
  }
  if ((*env)->ExceptionCheck(env)) {
    return 0;
  }

  (*env)->SetLongField(env, impl, gw_jvm.handle, (jlong)library->handle);
  (*env)->SetIntField(env, impl, gw_jvm.version, library->jni_version);
  if (gw_jvm.shape->has_loaded_field) {
    (*env)->SetBooleanField(env, impl, gw_jvm.loaded, JNI_TRUE);
  }

  return !(*env)->ExceptionCheck(env);
}

/* Loads through the Guest backend with GW_OPEN_AUTO: unmarked host images
 * return GW_PASS for the caller's original path. A publish failure rolls
 * the backend load back. */
int gw_jvm_open(JNIEnv *env, jobject impl, jstring name, int throw_if_fail) {
  const char *path = (*env)->GetStringUTFChars(env, name, NULL);
  if (!path) {
    return GW_ERROR;
  }

  struct gw_loader_library library = {0};
  struct gw_loader_error error = {0};
  int rc = gw_jvm.backend->open(env, path, GW_OPEN_AUTO, &library, &error);
  if (rc == GW_OK) {
    printf(
        "LOAD_PROXY operation=load target=glibc handle=%llx version=%x "
        "path=%s\n",
        (unsigned long long)library.handle, library.jni_version, path);
  }
  (*env)->ReleaseStringUTFChars(env, name, path);

  if (rc == GW_OK && !publish_library(env, impl, &library)) {
    rollback_open(env, library.handle);
    return GW_ERROR;
  }

  /* throwIfFail controls loader-not-found, not JNI_OnLoad/version rejection. */
  if (rc != GW_OK && rc != GW_PASS &&
      (throw_if_fail || error.code != GW_E_LOAD)) {
    gw_jvm_throw(env, &error);
  }

  return rc;
}

/* GW_PASS means the id is not backend-owned and the proxy still calls the
 * original unload; the owner entry is dropped only after a real close. */
int gw_jvm_close(JNIEnv *env, uint64_t id) {
  struct gw_loader_error error = {0};
  int rc = gw_jvm.backend->close(env, id, &error);

  if (rc == GW_OK) {
    (*env)->CallStaticVoidMethod(env, gw_jvm.helper, gw_jvm.forget, (jlong)id);
    if ((*env)->ExceptionCheck(env)) {
      return GW_ERROR;
    }
  } else if (rc != GW_PASS) {
    gw_jvm_throw(env, &error);
  }

  return rc;
}

/* Resolves one export through the backend. Symbol and full descriptor are
 * both required: short JNI names do not encode parameter types. Only hard
 * errors throw; a missing symbol yields NULL like dlsym. */
static void *resolve_entry(JNIEnv *env, uint64_t id, jstring name,
                           jstring descriptor) {
  const char *symbol = (*env)->GetStringUTFChars(env, name, NULL);
  const char *signature =
      symbol ? (*env)->GetStringUTFChars(env, descriptor, NULL) : NULL;
  void *entry = NULL;
  struct gw_loader_error error = {0};
  int rc = GW_ERROR;

  if (signature) {
    rc = gw_jvm.backend->find(env, id, symbol, signature, &entry, &error);
    (*env)->ReleaseStringUTFChars(env, descriptor, signature);
  }
  if (symbol) {
    (*env)->ReleaseStringUTFChars(env, name, symbol);
  }

  if (rc == GW_ERROR) {
    gw_jvm_throw(env, &error);
  }

  return rc == GW_OK ? entry : NULL;
}

/* findEntry0 for Guest handles. A module that is not LIVE raises instead
 * of returning a dead address; the descriptor is reflected from the
 * remembered owner class, then the backend resolves the entry. */
jlong gw_jvm_find(JNIEnv *env, uint64_t id, jstring name) {
  if (gw_jvm.backend->module_state(id) != GW_MODULE_LIVE) {
    struct gw_loader_error error = {GW_E_HANDLE,
                                    "Guest library is stale or closing"};
    gw_jvm_throw(env, &error);
    return 0;
  }

  jstring descriptor = (*env)->CallStaticObjectMethod(
      env, gw_jvm.helper, gw_jvm.signature, name, (jlong)id);
  if ((*env)->ExceptionCheck(env) || !descriptor) {
    return 0;
  }

  void *entry = resolve_entry(env, id, name, descriptor);
  (*env)->DeleteLocalRef(env, descriptor);

  return (jlong)(uintptr_t)entry;
}
