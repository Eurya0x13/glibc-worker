#include <jni.h>
#include <stddef.h>
#include <stdint.h>

static int failed(JNIEnv *env, const char *message) {
  jclass type = (*env)->FindClass(env, "java/lang/AssertionError");
  if (type) {
    (*env)->ThrowNew(env, type, message);
    (*env)->DeleteLocalRef(env, type);
  }
  return -1;
}

/* Exercise the public JNI contract through the same DSO in both domains. */
JNIEXPORT jint JNICALL Java_benchmark_PayloadModule_arrayChecks(
    JNIEnv *env, jclass cls, jobjectArray arrays, jthrowable expected) {
  (void)cls;
  int checks = 0;

#define CHECK(test)              \
  do {                           \
    if (!(test)) {               \
      return failed(env, #test); \
    }                            \
    checks++;                    \
  } while (0)

#define TEST_ARRAY(Type, type, index)                                         \
  do {                                                                        \
    j##type##Array a =                                                        \
        (j##type##Array)(*env)->GetObjectArrayElement(env, arrays, index);    \
    jsize n = (*env)->GetArrayLength(env, a);                                 \
    jboolean copy = JNI_FALSE;                                                \
    j##type *p = (*env)->Get##Type##ArrayElements(env, a, &copy),             \
            value[2] = {0};                                                   \
    CHECK(p != NULL);                                                         \
    CHECK(p[0] == 0 && p[n - 1] == 0);                                        \
    p[0] = p[n - 1] = 1;                                                      \
    (*env)->Release##Type##ArrayElements(env, a, p, JNI_COMMIT);              \
    (*env)->Get##Type##ArrayRegion(env, a, 0, 1, value);                      \
    CHECK(value[0] == 1);                                                     \
    if (!copy) {                                                              \
      p = (*env)->Get##Type##ArrayElements(env, a, &copy);                    \
      if (!p) {                                                               \
        return failed(env, "Cannot reacquire a direct array");                \
      }                                                                       \
    }                                                                         \
    jboolean second_copy = JNI_FALSE;                                         \
    j##type *second = (*env)->Get##Type##ArrayElements(env, a, &second_copy); \
    CHECK(second != NULL);                                                    \
    CHECK(!copy || !second_copy || p != second);                              \
    (*env)->Release##Type##ArrayElements(env, a, second, JNI_ABORT);          \
    p[0] = 0;                                                                 \
    (*env)->Release##Type##ArrayElements(env, a, p, JNI_ABORT);               \
    (*env)->Get##Type##ArrayRegion(env, a, 0, 1, value);                      \
    CHECK(value[0] == (copy ? 1 : 0));                                        \
    p = (*env)->Get##Type##ArrayElements(env, a, &copy);                      \
    CHECK(p != NULL);                                                         \
    p[0] = 0;                                                                 \
    (*env)->Throw(env, expected);                                             \
    (*env)->Release##Type##ArrayElements(env, a, p, JNI_COMMIT);              \
    jthrowable actual = (*env)->ExceptionOccurred(env);                       \
    (*env)->ExceptionClear(env);                                              \
    CHECK(actual && (*env)->IsSameObject(env, actual, expected));             \
    (*env)->DeleteLocalRef(env, actual);                                      \
    (*env)->Get##Type##ArrayRegion(env, a, 0, 1, value);                      \
    CHECK(value[0] == 0);                                                     \
    if (!copy) {                                                              \
      p = (*env)->Get##Type##ArrayElements(env, a, &copy);                    \
      if (!p) {                                                               \
        return failed(env, "Cannot reacquire a direct array");                \
      }                                                                       \
    }                                                                         \
    p[0] = 1;                                                                 \
    (*env)->Throw(env, expected);                                             \
    (*env)->Release##Type##ArrayElements(env, a, p, 0);                       \
    actual = (*env)->ExceptionOccurred(env);                                  \
    (*env)->ExceptionClear(env);                                              \
    CHECK(actual && (*env)->IsSameObject(env, actual, expected));             \
    (*env)->DeleteLocalRef(env, actual);                                      \
    (*env)->Get##Type##ArrayRegion(env, a, 0, 1, value);                      \
    CHECK(value[0] == 1);                                                     \
    p = (*env)->Get##Type##ArrayElements(env, a, NULL);                       \
    CHECK(p != NULL);                                                         \
    (*env)->Throw(env, expected);                                             \
    (*env)->Release##Type##ArrayElements(env, a, p, JNI_ABORT);               \
    actual = (*env)->ExceptionOccurred(env);                                  \
    (*env)->ExceptionClear(env);                                              \
    CHECK(actual && (*env)->IsSameObject(env, actual, expected));             \
    (*env)->DeleteLocalRef(env, actual);                                      \
    (*env)->DeleteLocalRef(env, a);                                           \
  } while (0)

  TEST_ARRAY(Boolean, boolean, 0);
  TEST_ARRAY(Byte, byte, 1);
  TEST_ARRAY(Char, char, 2);
  TEST_ARRAY(Short, short, 3);
  TEST_ARRAY(Int, int, 4);
  TEST_ARRAY(Long, long, 5);
  TEST_ARRAY(Float, float, 6);
  TEST_ARRAY(Double, double, 7);
#undef TEST_ARRAY
#undef CHECK

  return checks;
}

JNIEXPORT jlong JNICALL Java_benchmark_PayloadModule_arrayAcquire(
    JNIEnv *env, jclass cls, jbyteArray array) {
  (void)cls;
  jbyte *pointer = (*env)->GetByteArrayElements(env, array, NULL);
  if (pointer) {
    pointer[0] = 13;
    pointer[(*env)->GetArrayLength(env, array) - 1] = 27;
  }
  return (jlong)(uintptr_t)pointer;
}

JNIEXPORT void JNICALL Java_benchmark_PayloadModule_arrayRelease(
    JNIEnv *env, jclass cls, jbyteArray array, jlong pointer) {
  (void)cls;
  (*env)->ReleaseByteArrayElements(env, array, (jbyte *)(uintptr_t)pointer, 0);
}
