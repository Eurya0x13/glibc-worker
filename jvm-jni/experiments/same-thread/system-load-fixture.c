#include <jni.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <stddef.h>
#include <stdatomic.h>
#include <time.h>
#include <stdint.h>
#define FIXTURE_JNI_VERSION \
  0x00010008 /* JNI 1.8; neutral NDK header omits it. */
#ifndef REJECT_VERSION
#define REJECT_VERSION FIXTURE_JNI_VERSION
#endif
static jmethodID reply_method, recurse_method;

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *unused) {
  (void)unused;
  JNIEnv *env = NULL;
  if ((*vm)->GetEnv(vm, (void **)&env, FIXTURE_JNI_VERSION) != JNI_OK) {
    return JNI_ERR;
  }

  jclass fixture = (*env)->FindClass(env, "SystemLoadFixture");
  if (!fixture) {
    return JNI_ERR;
  }

  jmethodID method =
      (*env)->GetStaticMethodID(env, fixture, "onLoadCallback", "()I");
  if (!method) {
    return JNI_ERR;
  }

  jint answer = (*env)->CallStaticIntMethod(env, fixture, method);
  if ((*env)->ExceptionCheck(env) || answer != 73) {
    return JNI_ERR;
  }

  reply_method = (*env)->GetStaticMethodID(env, fixture, "reply", "(J)J");
  if (!reply_method) {
    return JNI_ERR;
  }
  recurse_method = (*env)->GetStaticMethodID(env, fixture, "recurse", "(I)I");
  if (!recurse_method) {
    return JNI_ERR;
  }
  return REJECT_VERSION;
}

JNIEXPORT void JNICALL JNI_OnUnload(JavaVM *vm, void *unused) {
  (void)vm;
  (void)unused;
}

JNIEXPORT jlong JNICALL Java_SystemLoadFixture_echo(JNIEnv *e, jclass c,
                                                    jlong x) {
  (void)e;
  (void)c;
  return x + 7;
}

JNIEXPORT jint JNICALL Java_SystemLoadFixture_overload__I(JNIEnv *e, jclass c,
                                                          jint x) {
  (void)e;
  (void)c;
  return x + 11;
}

JNIEXPORT jdouble JNICALL Java_SystemLoadFixture_overload__D(JNIEnv *e,
                                                             jclass c,
                                                             jdouble x) {
  (void)e;
  (void)c;
  return x + 0.5;
}

/* Deliberately hold one real Guest native frame across VM sampling and GC. */
static _Atomic int holding;

JNIEXPORT jint JNICALL Java_SystemLoadFixture_hold(JNIEnv *env, jclass cls) {
  (void)env;
  (void)cls;
  struct timespec start, now;
  clock_gettime(CLOCK_MONOTONIC, &start);
  atomic_store(&holding, 1);
  while (atomic_load(&holding) == 1) {
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec - start.tv_sec >= 15) {
      atomic_store(&holding, -1);
      return -1;
    }
  }
  return atomic_load(&holding) == 2 ? 73 : -1;
}

JNIEXPORT jint JNICALL Java_SystemLoadFixture_holdState(JNIEnv *env, jclass cls,
                                                        jint release) {
  (void)env;
  (void)cls;
  if (release) {
    atomic_store(&holding, 2);
  }
  return atomic_load(&holding);
}

/* Deterministic adopted-carrier signal delivery, separate from JFR's sampling
 * policy. The existing HotSpot fixture already uses this idle SR/PIPE/XFSZ
 * probe; the real worker backend intentionally must not run it. */
JNIEXPORT jint JNICALL Java_SystemLoadFixture_probeSignals(JNIEnv *env,
                                                           jclass cls) {
  (void)env;
  (void)cls;
  pthread_t self = pthread_self();
  const int numbers[] = {SIGUSR2, SIGPIPE, SIGXFSZ};
  for (unsigned i = 0; i < sizeof numbers / sizeof numbers[0]; ++i) {
    errno = EDOM;
    if (raise(numbers[i]) || errno != EDOM ||
        !pthread_equal(self, pthread_self())) {
      return -1;
    }
  }
  return 3;
}

JNIEXPORT jlong JNICALL Java_SystemLoadFixture_callback(JNIEnv *env, jclass cls,
                                                        jlong value) {
  return (*env)->CallStaticLongMethod(env, cls, reply_method, value);
}

JNIEXPORT jint JNICALL Java_SystemLoadFixture_nested(JNIEnv *env, jclass cls,
                                                     jint depth) {
  return depth ? (*env)->CallStaticIntMethod(env, cls, recurse_method,
                                             depth - 1) +
                     1
               : 0;
}

JNIEXPORT jint JNICALL Java_SystemLoadFixture_array(JNIEnv *env, jclass cls,
                                                    jbyteArray array) {
  (void)cls;
  jsize length = (*env)->GetArrayLength(env, array);
  jbyte *bytes = (*env)->GetByteArrayElements(env, array, NULL);
  if (!bytes) {
    return -1;
  }
  jint sum = 0;
  for (jsize i = 0; i < length; ++i) {
    sum += bytes[i];
  }
  (*env)->ReleaseByteArrayElements(env, array, bytes, JNI_ABORT);
  return sum;
}

JNIEXPORT jlong JNICALL Java_SystemLoadFixture_compute(JNIEnv *env, jclass cls,
                                                       jlong value) {
  (void)env;
  (void)cls;
  uint64_t x = (uint64_t)value;
  for (int i = 0; i < 10000; ++i) {
    x = x * 1664525 + 1013904223;
  }
  return (jlong)x;
}
