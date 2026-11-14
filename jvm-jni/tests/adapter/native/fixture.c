#include <jni.h>
#ifndef REJECT_VERSION
#define REJECT_VERSION JNI_VERSION_1_8
#endif
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *unused) {
  (void)unused;
  JNIEnv *env = NULL;
  if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_8) != JNI_OK) {
    return JNI_ERR;
  }

  jclass fixture = (*env)->FindClass(env, "AdapterFixture");
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

  return REJECT_VERSION;
}

JNIEXPORT void JNICALL JNI_OnUnload(JavaVM *vm, void *unused) {
  (void)vm;
  (void)unused;
}

JNIEXPORT jlong JNICALL Java_AdapterFixture_echo(JNIEnv *e, jclass c, jlong x) {
  (void)e;
  (void)c;
  return x + 7;
}

JNIEXPORT jint JNICALL Java_AdapterFixture_overload__I(JNIEnv *e, jclass c,
                                                       jint x) {
  (void)e;
  (void)c;
  return x + 11;
}

JNIEXPORT jdouble JNICALL Java_AdapterFixture_overload__D(JNIEnv *e, jclass c,
                                                          jdouble x) {
  (void)e;
  (void)c;
  return x + 0.5;
}
