#include <jni.h>

JNIEXPORT jint JNICALL Java_conformance_Module_hostControl(JNIEnv *env,
                                                           jclass cls,
                                                           jint value) {
  (void)env;
  (void)cls;
  return value + 100;
}
