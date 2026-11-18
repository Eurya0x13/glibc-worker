/* Adapter integration belongs to jvm-jni; transport remains in android-jni. */
#include "host_internal.h"
#include "jvm_adapter.h"
#include <stdlib.h>
#include <sys/resource.h>

static jintArray statistics(JNIEnv *env, jclass cls) {
  (void)cls;
  struct full_bridge *b = gw_host_bridge;
  jint values[] = {
      atomic_load(&b->loads),          atomic_load(&b->unloads),
      atomic_load(&b->active_modules), atomic_load(&b->image_opens),
      atomic_load(&b->image_closes),   atomic_load(&b->failed_loads),
      atomic_load(&b->live_closures)};
  jintArray result = (*env)->NewIntArray(env, 7);
  if (result) {
    (*env)->SetIntArrayRegion(env, result, 0, 7, values);
  }
  return result;
}

/* Process-wide CPU and switch counts include Host and Guest threads. Sample
 * outside timed calls; report background VM/ART work rather than hiding it. */
static jlongArray metrics(JNIEnv *env, jclass cls) {
  (void)cls;
  struct rusage usage;
  if (getrusage(RUSAGE_SELF, &usage)) {
    return NULL;
  }
  jlong values[] = {
      ((jlong)usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1000000000 +
          ((jlong)usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) * 1000,
      usage.ru_nvcsw, usage.ru_nivcsw};
  jlongArray result = (*env)->NewLongArray(env, 3);
  if (result) {
    (*env)->SetLongArrayRegion(env, result, 0, 3, values);
  }
  return result;
}

int domain_system_load_run(JNIEnv *env) {
  jclass cls = (*env)->FindClass(env, "SystemLoadProbe");
  if (!cls) {
    return 0;
  }
  JNINativeMethod methods[] = {{"stats", "()[I", (void *)statistics},
                               {"metrics", "()[J", (void *)metrics}};
  if ((*env)->RegisterNatives(env, cls, methods, 2) != JNI_OK) {
    return 0;
  }
  jmethodID run =
      (*env)->GetStaticMethodID(env, cls, "run",
                                "(Ljava/lang/String;Ljava/lang/String;Ljava/"
                                "lang/String;Ljava/lang/String;)I");
  if (!run) {
    return 0;
  }
  const char *names[] = {"GW_SYSTEM_JAR", "GW_SYSTEM_IMAGE", "GW_SYSTEM_REJECT",
                         "GW_SYSTEM_ORDINARY"};
  jstring args[4] = {0};
  for (int i = 0; i < 4; ++i) {
    const char *value = getenv(names[i]);
    if (!value || !(args[i] = (*env)->NewStringUTF(env, value))) {
      return 0;
    }
  }
  jint result = (*env)->CallStaticIntMethod(env, cls, run, args[0], args[1],
                                            args[2], args[3]);
  if ((*env)->ExceptionCheck(env)) {
    (*env)->ExceptionDescribe(env);
    return 0;
  }
  for (int i = 0; i < 4; ++i) {
    (*env)->DeleteLocalRef(env, args[i]);
  }
  (*env)->DeleteLocalRef(env, cls);
  return result == 1;
}
