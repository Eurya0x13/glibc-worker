#include <jni.h>
#include <stdio.h>
extern int missing_dependency(void);

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *opaque) {
  (void)vm;
  (void)opaque;
  dprintf(2, "UNEXPECTED_MISSING_DEPENDENCY_ONLOAD\n");
  return missing_dependency();
}
