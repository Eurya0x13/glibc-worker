#include "image_policy.h"
#include "internal/jni_bytes.h"
#include "loader_backend_internal.h"
#include <stdio.h>
#include <string.h>

/* The class ref is promoted to a global before anything else can fail, and
   configure runs exactly once here; only the resolve methodID is retained
   afterwards. All failures funnel through finish so a failed start leaves
   nothing pinned. */
int gw_image_policy_start(struct gw_image_policy *policy, JNIEnv *env,
                          const char *directory, const char *catalog,
                          int allow_direct, struct gw_loader_error *error) {
  memset(policy, 0, sizeof *policy);

  jclass helper = (*env)->FindClass(env, "dev/glibc/worker/GuestImages");
  if (!helper) {
    return gw_loader_fail(error, GW_E_IMAGE,
                          "GuestImages helper not available");
  }

  policy->helper = (jclass)(*env)->NewGlobalRef(env, helper);
  (*env)->DeleteLocalRef(env, helper);
  if (!policy->helper) {
    return gw_loader_fail(error, GW_E_IMAGE,
                          "Cannot retain image policy helper");
  }

  jmethodID configure = (*env)->GetStaticMethodID(
      env, policy->helper, "configureNative", "([B[BZ)V");
  if (!configure) {
    goto fail;
  }
  policy->resolve =
      (*env)->GetStaticMethodID(env, policy->helper, "resolveNative", "([B)[B");
  if (!policy->resolve) {
    goto fail;
  }

  jbyteArray dir = gw_jni_new_bytes(env, directory);
  if (!dir) {
    goto fail;
  }
  jbyteArray file = gw_jni_new_bytes(env, catalog);
  if (!file) {
    (*env)->DeleteLocalRef(env, dir);
    goto fail;
  }

  (*env)->CallStaticVoidMethod(env, policy->helper, configure, dir, file,
                               (jboolean)allow_direct);
  (*env)->DeleteLocalRef(env, file);
  (*env)->DeleteLocalRef(env, dir);
  if ((*env)->ExceptionCheck(env)) {
    goto fail;
  }

  printf("LOADER_IMAGE_POLICY configured=1 mode=%s\n",
         allow_direct ? "DIRECT_CONTROL" : "INSTALLED_CATALOG");
  return GW_OK;

fail:
  gw_image_policy_finish(policy, env);
  return gw_loader_fail(error, GW_E_IMAGE, "Image policy configuration failed");
}

/* Called for every guest image request on a thread with a live JNIEnv. A
   null or throwing resolve is a rejection (GW_E_IMAGE); a Java-side
   rejection exception stays pending in the caller's frame. The returned
   path must fit the fixed GW_LOADER_PATH buffer or the request fails. */
int gw_image_policy_resolve(JNIEnv *env, const char *path,
                            char image[GW_LOADER_PATH],
                            struct gw_loader_error *error, void *context) {
  struct gw_image_policy *policy = context;
  jbyteArray requested = gw_jni_new_bytes(env, path);
  if (!requested) {
    return gw_loader_fail(error, GW_E_IMAGE, "Cannot allocate image path");
  }

  jbyteArray value = (jbyteArray)(*env)->CallStaticObjectMethod(
      env, policy->helper, policy->resolve, requested);
  (*env)->DeleteLocalRef(env, requested);
  if ((*env)->ExceptionCheck(env) || !value) {
    return gw_loader_fail(error, GW_E_IMAGE,
                          "Image policy rejected the requested library");
  }

  bool valid = gw_jni_read_path(env, value, image, GW_LOADER_PATH);
  (*env)->DeleteLocalRef(env, value);
  if (!valid) {
    return gw_loader_fail(error, GW_E_IMAGE,
                          "Image policy returned an invalid native path");
  }
  return GW_OK;
}

/* Must run on a thread attached to the same VM that started the policy;
   safe to call again after a zeroed finish. */
void gw_image_policy_finish(struct gw_image_policy *policy, JNIEnv *env) {
  if (policy->helper) {
    (*env)->DeleteGlobalRef(env, policy->helper);
  }
  memset(policy, 0, sizeof *policy);
}
