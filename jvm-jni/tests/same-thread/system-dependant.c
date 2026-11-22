#include <jni.h>

extern void system_dependency_note_load(void);
extern int system_dependency_loads(void);
extern int system_dependency_value(void);

__attribute__((constructor)) static void note_load(void) {
  system_dependency_note_load();
}

JNIEXPORT jint JNICALL Java_SystemLoadFixture_dependencyLoads(JNIEnv *env,
                                                              jclass cls) {
  (void)env;
  (void)cls;
  return system_dependency_loads();
}

JNIEXPORT jint JNICALL Java_SystemLoadFixture_dependencyValue(JNIEnv *env,
                                                              jclass cls) {
  (void)env;
  (void)cls;
  return system_dependency_value();
}
