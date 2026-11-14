#define _GNU_SOURCE
#include "adapter_internal.h"
#include "jre17_adapter.h"
#include "jni_vm_adapter.h"
#include "dlopen_backend.h"
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

/* Drops every JNI reference, the libjava handle and all resolved ids. Safe
 * only when no proxy is reachable from the VM: install never happened, or
 * restore re-registered every original. Runs with gw_jvm.lock held. */
static void release(JNIEnv *env) {
  for (unsigned i = 0; i < 3; i++) {
    if (gw_jvm.hooks[i].owner) {
      (*env)->DeleteGlobalRef(env, gw_jvm.hooks[i].owner);
    }
    memset(&gw_jvm.hooks[i], 0, sizeof gw_jvm.hooks[i]);
  }

  if (gw_jvm.helper) {
    (*env)->DeleteGlobalRef(env, gw_jvm.helper);
  }
  gw_jvm.helper = NULL;

  if (gw_jvm.libjava) {
    dlclose(gw_jvm.libjava);
  }
  gw_jvm.libjava = NULL;

  gw_jvm.handle = gw_jvm.version = gw_jvm.from = gw_jvm.loaded = NULL;
  gw_jvm.verify = gw_jvm.remember = gw_jvm.forget = gw_jvm.signature = NULL;
  gw_jvm.shape = NULL;
  gw_jvm.backend = NULL;
}

/* Pins JvmAdapter and its four static helpers (digest check, owner
 * bookkeeping, signature reflection) before anything is hooked, so a
 * missing helper fails install with the VM untouched. */
static int resolve_helper(JNIEnv *env) {
  jclass local = (*env)->FindClass(env, "dev/glibc/worker/JvmAdapter");
  if (!local) {
    return 0;
  }

  gw_jvm.helper = (*env)->NewGlobalRef(env, local);
  (*env)->DeleteLocalRef(env, local);
  if (!gw_jvm.helper) {
    return 0;
  }

  gw_jvm.verify =
      (*env)->GetStaticMethodID(env, gw_jvm.helper, "verifyLibrary",
                                "(Ljava/lang/String;Ljava/lang/String;)Z");
  if (!gw_jvm.verify) {
    return 0;
  }

  gw_jvm.remember = (*env)->GetStaticMethodID(env, gw_jvm.helper, "remember",
                                              "(JLjava/lang/Class;)V");
  if (!gw_jvm.remember) {
    return 0;
  }

  gw_jvm.forget =
      (*env)->GetStaticMethodID(env, gw_jvm.helper, "forget", "(J)V");
  if (!gw_jvm.forget) {
    return 0;
  }

  gw_jvm.signature =
      (*env)->GetStaticMethodID(env, gw_jvm.helper, "nativeSignature",
                                "(Ljava/lang/String;J)Ljava/lang/String;");
  return gw_jvm.signature != NULL;
}

/* Delegates hashing to the Java helper and compares with the profile
 * digest; any pending exception counts as a mismatch. Only past this gate
 * is libjava dlopened. */
static int verify_image(JNIEnv *env, const char *path) {
  jstring file = (*env)->NewStringUTF(env, path);
  if (!file) {
    return 0;
  }

  jstring digest = (*env)->NewStringUTF(env, gw_jvm.profile.libjava_sha256);
  jboolean matches = JNI_FALSE;
  if (digest) {
    matches = (*env)->CallStaticBooleanMethod(env, gw_jvm.helper, gw_jvm.verify,
                                              file, digest);
    (*env)->DeleteLocalRef(env, digest);
  }

  (*env)->DeleteLocalRef(env, file);
  return !(*env)->ExceptionCheck(env) && matches;
}

/* Resolves the fields the proxies publish into. handle, jniVersion and
 * fromClass are resolved unconditionally, `loaded` only when the family's
 * shape asks for it. The owning class comes from the shape. */
static int resolve_fields(JNIEnv *env) {
  jclass impl = (*env)->FindClass(env, gw_jvm.shape->implementation);
  if (!impl) {
    return 0;
  }

  gw_jvm.handle = (*env)->GetFieldID(env, impl, "handle", "J");
  if (gw_jvm.handle) {
    gw_jvm.version = (*env)->GetFieldID(env, impl, "jniVersion", "I");
  }
  if (gw_jvm.version) {
    gw_jvm.from =
        (*env)->GetFieldID(env, impl, "fromClass", "Ljava/lang/Class;");
  }
  if (gw_jvm.from && gw_jvm.shape->has_loaded_field) {
    gw_jvm.loaded = (*env)->GetFieldID(env, impl, "loaded", "Z");
  }

  (*env)->DeleteLocalRef(env, impl);
  return gw_jvm.handle && gw_jvm.version && gw_jvm.from &&
         (!gw_jvm.shape->has_loaded_field || gw_jvm.loaded);
}

/* Resolves all three slots completely before anything is registered: the
 * original export via dlsym, the pinned owner class, and the Java method
 * by exact name and descriptor. An unexpected libjava leaves the VM
 * untouched. */
static int resolve_hooks(JNIEnv *env) {
  for (unsigned i = 0; i < 3; i++) {
    const struct gw_jvm_hook_shape *shape = &gw_jvm.shape->hooks[i];
    struct gw_jvm_hook *hook = &gw_jvm.hooks[i];
    void *original = dlsym(gw_jvm.libjava, shape->symbol);
    if (!original) {
      return 0;
    }

    jclass local = (*env)->FindClass(env, shape->owner);
    if (!local) {
      return 0;
    }
    hook->owner = (*env)->NewGlobalRef(env, local);
    (*env)->DeleteLocalRef(env, local);
    if (!hook->owner) {
      return 0;
    }

    jmethodID method =
        shape->is_static ? (*env)->GetStaticMethodID(
                               env, hook->owner, shape->name, shape->descriptor)
                         : (*env)->GetMethodID(env, hook->owner, shape->name,
                                               shape->descriptor);
    if (!method) {
      return 0;
    }

    hook->replacement = (JNINativeMethod){
        (char *)shape->name, (char *)shape->descriptor, shape->proxy};
    hook->original = hook->replacement;
    hook->original.fnPtr = original;
  }

  return 1;
}

/* Try every original, including registrations after a partial failure, and
 * preserve the first exception. Failed restoration keeps all state alive. */
static int restore_hooks(JNIEnv *env) {
  jthrowable cause = (*env)->ExceptionOccurred(env);
  if (cause) {
    (*env)->ExceptionClear(env);
  }

  int ok = 1;
  for (unsigned i = 0; i < 3; i++) {
    struct gw_jvm_hook *hook = &gw_jvm.hooks[i];
    if ((*env)->RegisterNatives(env, hook->owner, &hook->original, 1) !=
        JNI_OK) {
      ok = 0;
    }
    if ((*env)->ExceptionCheck(env)) {
      if (!cause) {
        cause = (*env)->ExceptionOccurred(env);
      }
      (*env)->ExceptionClear(env);
      ok = 0;
    }
  }

  if (cause) {
    (*env)->Throw(env, cause);
    (*env)->DeleteLocalRef(env, cause);
  }

  return ok;
}

/* Registers the three replacements; the first failure rolls everything back
 * to the originals. If that rollback also fails, `installed` is set so
 * release() cannot free state the live proxies still use. */
static int install_hooks(JNIEnv *env) {
  for (unsigned i = 0; i < 3; i++) {
    struct gw_jvm_hook *hook = &gw_jvm.hooks[i];
    if ((*env)->RegisterNatives(env, hook->owner, &hook->replacement, 1) !=
            JNI_OK ||
        (*env)->ExceptionCheck(env)) {
      int rollback = restore_hooks(env);
      printf("LOAD_PROXY install_rollback=%d\n", rollback);
      if (!rollback) {
        gw_jvm.installed = 1;
      }
      return 0;
    }
  }

  gw_jvm.installed = 1;
  return 1;
}

/* Install pipeline: backend and shape lookup, helper, image digest, then
 * field and hook resolution from libjava, then registration. Every check
 * completes before the first RegisterNatives, so failure leaves the VM
 * untouched or fully restored. */
static int prepare(JNIEnv *env, const char *path,
                   struct gw_loader_error *error) {
  gw_jvm.backend = gw_loader_query(GW_LOADER_ABI_V1, sizeof *gw_jvm.backend);
  gw_jvm.shape = gw_jvm_shape(gw_jvm.profile.family);
  if (!gw_jvm.backend || !gw_jvm.shape || !resolve_helper(env)) {
    return gw_dlopen_fail(error, GW_E_BRIDGE,
                          "Cannot resolve JVM adapter helper");
  }

  if (!verify_image(env, path)) {
    return gw_dlopen_fail(error, GW_E_IMAGE,
                          "JVM adapter profile does not match libjava SHA256");
  }

  gw_jvm.libjava = dlopen(path, RTLD_NOW);
  if (!gw_jvm.libjava || !resolve_fields(env) || !resolve_hooks(env)) {
    return gw_dlopen_fail(
        error, GW_E_BRIDGE,
        "JVM classes, fields or native methods differ from profile");
  }

  if (!install_hooks(env)) {
    return gw_dlopen_fail(
        error, GW_E_BRIDGE,
        gw_jvm.installed
            ? "JVM adapter rollback failed; process disposal required"
            : "JVM adapter registration failed and originals restored");
  }

  return GW_OK;
}

/* Single install entry under the adapter lock. A failed prepare releases
 * all state, unless a failed rollback left the proxies installed: that
 * outcome is reported for process disposal instead. */
int gw_jvm_adapter_install(JNIEnv *env, const char *directory,
                           const char *library, struct gw_loader_error *error) {
  if (!env || (!directory && !library)) {
    return gw_dlopen_fail(error, GW_E_ARGUMENT,
                          "JVM environment and path required");
  }

  char path[GW_LOADER_PATH];
  int n = library ? snprintf(path, sizeof path, "%s", library)
                  : snprintf(path, sizeof path, "%s/libjava.so", directory);
  if (n < 0 || n >= (int)sizeof path) {
    return gw_dlopen_fail(error, GW_E_ARGUMENT, "libjava path too long");
  }

  pthread_mutex_lock(&gw_jvm.lock);
  int rc = GW_ERROR;
  if (gw_jvm.installed) {
    rc = gw_dlopen_fail(error, GW_E_BUSY, "JVM adapter is installed");
  } else {
    rc = prepare(env, path, error);
    if (rc != GW_OK && !gw_jvm.installed) {
      release(env);
    }
  }

  if (rc == GW_OK) {
    printf("LOAD_PROXY installed=1 family=%u profile=%s libjava_sha256=%s\n",
           gw_jvm.profile.family, gw_jvm.profile.name,
           gw_jvm.profile.libjava_sha256);
  }

  pthread_mutex_unlock(&gw_jvm.lock);
  return rc;
}

/* Re-registers the three originals. Only on full success are hooks, helper
 * references and libjava released and the adapter reusable; a failed
 * restore keeps everything for a retry. */
int gw_jvm_adapter_restore(JNIEnv *env, struct gw_loader_error *error) {
  if (!env) {
    return gw_dlopen_fail(error, GW_E_ARGUMENT, "JVM environment required");
  }

  pthread_mutex_lock(&gw_jvm.lock);
  int rc = GW_OK;
  if (gw_jvm.installed) {
    int ok = restore_hooks(env);
    printf("LOAD_PROXY restored=%d\n", ok);
    if (ok) {
      gw_jvm.installed = 0;
      release(env);
    } else {
      rc = gw_dlopen_fail(error, GW_E_BRIDGE,
                          "Cannot restore JVM native methods");
    }
  }

  pthread_mutex_unlock(&gw_jvm.lock);
  return rc;
}

/* Source and binary compatibility for existing JRE17 embedders. */
int gw_jre17_adapter_install(JNIEnv *e, const char *d, const char *p,
                             struct gw_loader_error *error) {
  return gw_jvm_adapter_install(e, d, p, error);
}

/* Restore counterpart of the compatibility entry above. */
int gw_jre17_adapter_restore(JNIEnv *e, struct gw_loader_error *error) {
  return gw_jvm_adapter_restore(e, error);
}
/* v1 VM-adapter table; registration keeps this pointer until process exit,
 * so it must have static storage. */
static const struct gw_jni_vm_adapter_v1 adapter = {
    GW_JNI_VM_ADAPTER_V1, sizeof adapter, gw_jvm_adapter_install,
    gw_jvm_adapter_restore};

/* Must run before the public bridge starts: afterwards the adapter set is
 * frozen. Until then, re-registering the same table stays GW_OK. */
int gw_jvm_jni_register(void) {
  return gw_jni_register_vm_adapter(&adapter);
}

/* Auto-register at DSO load; hosts that never call gw_jvm_jni_register get
 * the same table. */
__attribute__((constructor)) static void register_adapter(void) {
  if (gw_jvm_jni_register() != GW_OK) {
    fprintf(stderr, "JVM_JNI adapter_registration_failed=1\n");
  }
}
