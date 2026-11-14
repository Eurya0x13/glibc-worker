#include "host_internal.h"

/* Error paths only. Never replace an application's pending exception. Call
   outside registry locks: FindClass may run VM code even for core classes.
   An unmapped kind reports IllegalStateException, and when the VM cannot
   furnish the class the function exits with whatever exception that lookup
   left pending. */
void fh_throw(JNIEnv *env, enum fb_failure kind, const char *message) {
  if (!env || (*env)->ExceptionCheck(env)) {
    return;
  }

  const char *name = kind == FB_FAILURE_MEMORY  ? "java/lang/OutOfMemoryError"
                     : kind == FB_FAILURE_DEPTH ? "java/lang/StackOverflowError"
                     : kind == FB_FAILURE_ARGUMENT
                         ? "java/lang/IllegalArgumentException"
                         : "java/lang/IllegalStateException";
  jclass cls = (*env)->FindClass(env, name);
  if (cls) {
    (*env)->ThrowNew(env, cls, message);
    (*env)->DeleteLocalRef(env, cls);
  }
}
