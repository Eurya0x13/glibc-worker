#include <jni.h>
#include <unistd.h>

static jmethodID reply, bounce;

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
  (void)reserved;
  JNIEnv *env = NULL;
  if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
    return JNI_ERR;
  }

  jclass type = (*env)->FindClass(env, "benchmark/ChildModule");
  if (!type) {
    return JNI_ERR;
  }
  reply = (*env)->GetStaticMethodID(env, type, "reply", "(I)I");
  bounce = (*env)->GetStaticMethodID(env, type, "bounce", "(I)I");
  (*env)->DeleteLocalRef(env, type);
  return reply && bounce ? JNI_VERSION_1_6 : JNI_ERR;
}

JNIEXPORT jint JNICALL Java_benchmark_ChildModule_echo(JNIEnv *env, jclass type,
                                                       jint value) {
  (void)env;
  (void)type;
  return value ^ 0x13579;
}

JNIEXPORT jint JNICALL Java_benchmark_ChildModule_roundtrip(JNIEnv *env,
                                                            jclass type,
                                                            jint value) {
  return (*env)->CallStaticIntMethod(env, type, reply, value);
}

JNIEXPORT jint JNICALL Java_benchmark_ChildModule_nested(JNIEnv *env,
                                                         jclass type,
                                                         jint depth) {
  return (*env)->CallStaticIntMethod(env, type, bounce, depth);
}

JNIEXPORT void JNICALL Java_benchmark_ChildModule_hold(JNIEnv *env, jclass type,
                                                       jint milliseconds) {
  (void)env;
  (void)type;
  usleep((unsigned)milliseconds * 1000);
}
