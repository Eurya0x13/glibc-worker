#include <jni.h>
#include <stdio.h>

JNIEXPORT jint JNICALL Java_ArrayCommitControl_check(JNIEnv *env, jclass cls) {
  (void)cls;
  jbooleanArray array = (*env)->NewBooleanArray(env, 3);
  if (!array) {
    return 1;
  }

  jboolean copy = JNI_FALSE;
  jboolean *elements = (*env)->GetBooleanArrayElements(env, array, &copy);
  if (!elements) {
    return 2;
  }

  elements[0] = JNI_TRUE;
  (*env)->ReleaseBooleanArrayElements(env, array, elements, JNI_COMMIT);
  fprintf(stderr, "ARRAY_COMMIT_CONTROL stage=commit copy=%d\n", copy);
  fflush(stderr);

  // JNI_COMMIT retains a copied buffer until the final release. A direct
  // buffer may already be unpinned, so reacquire that case before using it.
  if (!copy) {
    elements = (*env)->GetBooleanArrayElements(env, array, &copy);
  }
  if (!elements) {
    return 3;
  }

  elements[1] = JNI_TRUE;
  (*env)->ReleaseBooleanArrayElements(env, array, elements, 0);

  jboolean result[3] = {0};
  (*env)->GetBooleanArrayRegion(env, array, 0, 3, result);
  if ((*env)->ExceptionCheck(env)) {
    return 4;
  }

  (*env)->DeleteLocalRef(env, array);
  return result[0] == JNI_TRUE && result[1] == JNI_TRUE ? 0 : 5;
}
