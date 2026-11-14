/* Valid GNU/glibc DSOs; no bridge ABI dependency. */
#include <jni.h>
#include <stdatomic.h>
#include <stdio.h>
static _Atomic int *counter;

JNIEXPORT void conformance_observe_destructor(_Atomic int *p) {
  counter = p;
}

__attribute__((destructor)) static void fini(void) {
  if (counter) {
    atomic_fetch_add(counter, 1);
  }
}
#ifdef CONTROL_VERSION
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
  (void)vm;
  (void)reserved;
  return CONTROL_VERSION;
}

JNIEXPORT void JNICALL JNI_OnUnload(JavaVM *vm, void *reserved) {
  (void)vm;
  (void)reserved;
  dprintf(2, "UNEXPECTED_REJECTED_ONUNLOAD\n");
}
#endif
