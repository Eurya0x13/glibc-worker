/* Conventional C JNI DSO: no bridge/protocol header or Host symbols. */
#include <jni.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdint.h>

static __thread int marker = 19;
static int status = -1;

static void *unbound_thread(void *arg) {
  JavaVM *vm = arg;
  void *env = (void *)(uintptr_t)1;
  jint rc = (*vm)->GetEnv(vm, &env, JNI_VERSION_1_6);
  return (void *)(uintptr_t)(rc == JNI_EDETACHED && env == NULL &&
                             marker == 19);
}

static jint invoke_v(JNIEnv *env, jclass cls, jmethodID method, ...) {
  va_list args;
  va_start(args, method);
  jint result = (*env)->CallStaticIntMethodV(env, cls, method, args);
  va_end(args);
  return result;
}

#define CHECK(condition, code) \
  do {                         \
    if (!(condition)) {        \
      status = (code);         \
      return JNI_ERR;          \
    }                          \
  } while (0)

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
  (void)reserved;
  JNIEnv *env = NULL;
  CHECK((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) == JNI_OK && env, 1);
  void *bad = (void *)(uintptr_t)1;
  CHECK((*vm)->GetEnv(vm, &bad, 0x7fffffff) == JNI_EVERSION && bad == NULL, 2);
  JavaVM *roundtrip = NULL;
  CHECK((*env)->GetJavaVM(env, &roundtrip) == JNI_OK && roundtrip == vm, 3);
  pthread_t thread;
  void *result = NULL;
  CHECK(pthread_create(&thread, NULL, unbound_thread, vm) == 0, 4);
  CHECK(pthread_join(thread, &result) == 0 && result == (void *)(uintptr_t)1,
        5);

  int *allocation = malloc(sizeof *allocation);
  CHECK(allocation, 6);
  *allocation = 51;
  errno = 51;
  marker = 67;
  uintptr_t before = (uintptr_t)__builtin_thread_pointer();

  CHECK((*env)->GetVersion(env) >= JNI_VERSION_1_6, 7);
  jclass cls = (*env)->FindClass(env, "dev/glibc/worker/NativeBridge");
  CHECK(cls && !(*env)->ExceptionCheck(env), 8);
  jmethodID method = (*env)->GetStaticMethodID(env, cls, "jniCallback", "(I)I");
  CHECK(method && !(*env)->ExceptionCheck(env), 9);
  CHECK((*env)->CallStaticIntMethod(env, cls, method, 7) == 21, 10);
  jvalue value = {.i = 11};
  CHECK((*env)->CallStaticIntMethodA(env, cls, method, &value) == 33, 11);
  CHECK(invoke_v(env, cls, method, 13) == 39, 12);

  jmethodID throwing =
      (*env)->GetStaticMethodID(env, cls, "jniThrowing", "(I)I");
  CHECK(throwing && !(*env)->ExceptionCheck(env), 13);
  value.i = 23;
  (void)(*env)->CallStaticIntMethodA(env, cls, throwing, &value);
  CHECK((*env)->ExceptionCheck(env), 14);
  (*env)->ExceptionClear(env);
  CHECK(!(*env)->ExceptionCheck(env), 15);

  (*env)->DeleteLocalRef(env, cls);
  CHECK(errno == 51 && marker == 67 &&
            (uintptr_t)__builtin_thread_pointer() == before &&
            *allocation == 51,
        16);

  free(allocation);
  status = 0;
  return JNI_VERSION_1_6;
}

JNIEXPORT jint JNICALL guest_jni_status(void) {
  return status;
}
