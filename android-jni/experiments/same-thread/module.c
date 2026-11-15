#include <jni.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

static JavaVM *saved_vm;
static jmethodID reply, recurse;
static _Atomic int pause_state;
static pthread_barrier_t guest_gate;

static jint echo(JNIEnv *env, jclass cls, jint value) {
  (void)env;
  (void)cls;
  return value ^ 0x13579;
}

static void pause_guest(JNIEnv *env, jclass cls) {
  (void)env;
  (void)cls;
  struct timespec left = {.tv_nsec = 300000000};
  atomic_store(&pause_state, 1);
  while (nanosleep(&left, &left) && errno == EINTR) {
  }
  atomic_store(&pause_state, 2);
}

static jint pause_status(JNIEnv *env, jclass cls) {
  (void)env;
  (void)cls;
  return atomic_load(&pause_state);
}

static void *guest_created(void *argument) {
  int daemon = (int)(intptr_t)argument;
  JNIEnv *env = NULL;
  jint rc = daemon
                ? (*saved_vm)->AttachCurrentThreadAsDaemon(saved_vm, &env, NULL)
                : (*saved_vm)->AttachCurrentThread(saved_vm, &env, NULL);
  if (rc != JNI_OK || !env) {
    _exit(121);
  }
  pthread_barrier_wait(&guest_gate);
  jclass cls = (*env)->FindClass(env, "JniDomainProbe");
  jmethodID nested =
      cls ? (*env)->GetStaticMethodID(env, cls, "nested", "(I)I") : NULL;
  int good = 0;
  if (nested) {
    jint result = (*env)->CallStaticIntMethod(env, cls, nested, 8);
    good = !(*env)->ExceptionCheck(env) && result == 8;
  }
  if ((*env)->ExceptionCheck(env)) {
    (*env)->ExceptionClear(env);
  }
  if (cls) {
    (*env)->DeleteLocalRef(env, cls);
  }
  good &= (*saved_vm)->DetachCurrentThread(saved_vm) == JNI_OK;
  void *again = (void *)1;
  good &=
      (*saved_vm)->GetEnv(saved_vm, &again, JNI_VERSION_1_6) == JNI_EDETACHED &&
      !again;
  return (void *)(intptr_t)good;
}

static jint guest_threads(JNIEnv *env, jclass cls) {
  (void)env;
  (void)cls;
  pthread_t tasks[32];
  if (pthread_barrier_init(&guest_gate, NULL, 32)) {
    return -1;
  }
  for (int i = 0; i < 32; ++i) {
    if (pthread_create(&tasks[i], NULL, guest_created,
                       (void *)(intptr_t)(i & 1))) {
      _exit(121);
    }
  }
  int good = 0;
  for (int i = 0; i < 32; ++i) {
    void *result = NULL;
    if (pthread_join(tasks[i], &result)) {
      _exit(121);
    }
    good += result == (void *)1;
  }
  pthread_barrier_destroy(&guest_gate);
  dprintf(1, "JNI_DOMAIN_GUEST_THREADS {\"created\":32,\"passed\":%d}\n", good);
  return good;
}

static jint callback(JNIEnv *env, jclass cls, jint value) {
  return (*env)->CallStaticIntMethod(env, cls, reply, value);
}

static jint nested(JNIEnv *env, jclass cls, jint depth) {
  return depth ? (*env)->CallStaticIntMethod(env, cls, recurse, depth - 1) + 1
               : 0;
}

static jobject identity(JNIEnv *env, jclass cls, jobject object) {
  (void)cls;
  if ((*env)->PushLocalFrame(env, 8) < 0) {
    return NULL;
  }
  jobject global = (*env)->NewGlobalRef(env, object);
  jweak weak = (*env)->NewWeakGlobalRef(env, object);
  jobject local = (*env)->NewLocalRef(env, weak);
  int same = (*env)->IsSameObject(env, global, local);
  (*env)->DeleteWeakGlobalRef(env, weak);
  (*env)->DeleteGlobalRef(env, global);
  return (*env)->PopLocalFrame(env, same ? local : NULL);
}

static jint array(JNIEnv *env, jclass cls, jbyteArray value) {
  (void)cls;
  jsize size = (*env)->GetArrayLength(env, value);
  jbyte *bytes = (*env)->GetByteArrayElements(env, value, NULL);
  if (!bytes) {
    return -1;
  }
  int result = 0;
  for (jsize i = 0; i < size; ++i) {
    result += (unsigned char)bytes[i];
  }
  (*env)->ReleaseByteArrayElements(env, value, bytes, JNI_ABORT);
  return result;
}

static jstring string(JNIEnv *env, jclass cls, jstring value) {
  (void)cls;
  const char *text = (*env)->GetStringUTFChars(env, value, NULL);
  if (!text) {
    return NULL;
  }
  jstring copy = (*env)->NewStringUTF(env, text);
  (*env)->ReleaseStringUTFChars(env, value, text);
  return copy;
}

static void throwing(JNIEnv *env, jclass cls) {
  (void)cls;
  jclass error = (*env)->FindClass(env, "java/lang/IllegalStateException");
  if (error) {
    (*env)->ThrowNew(env, error, "domain-probe");
  }
}

static jint vm_check(JNIEnv *env, jclass cls) {
  (void)cls;
  JavaVM *vm = NULL;
  JNIEnv *again = NULL;
  if ((*env)->GetJavaVM(env, &vm) || vm != saved_vm) {
    return 0;
  }
  if ((*vm)->GetEnv(vm, (void **)&again, JNI_VERSION_1_6) || again != env) {
    return 0;
  }
  if ((*vm)->AttachCurrentThread(vm, &again, NULL) || again != env) {
    return 0;
  }
  return 1;
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
  (void)reserved;
  saved_vm = vm;
  JNIEnv *env;
  if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6)) {
    return JNI_ERR;
  }
  jclass cls = (*env)->FindClass(env, "JniDomainProbe");
  if (!cls) {
    return JNI_ERR;
  }
  reply = (*env)->GetStaticMethodID(env, cls, "reply", "(I)I");
  recurse = (*env)->GetStaticMethodID(env, cls, "recurse", "(I)I");
  JNINativeMethod methods[] = {
      {"echo", "(I)I", (void *)echo},
      {"pauseGuest", "()V", (void *)pause_guest},
      {"pauseStatus", "()I", (void *)pause_status},
      {"guestThreads", "()I", (void *)guest_threads},
      {"callback", "(I)I", (void *)callback},
      {"nested", "(I)I", (void *)nested},
      {"identity", "(Ljava/lang/Object;)Ljava/lang/Object;", (void *)identity},
      {"array", "([B)I", (void *)array},
      {"string", "(Ljava/lang/String;)Ljava/lang/String;", (void *)string},
      {"throwing", "()V", (void *)throwing},
      {"vmCheck", "()I", (void *)vm_check}};
  if (!reply || !recurse ||
      (*env)->RegisterNatives(env, cls, methods,
                              sizeof methods / sizeof methods[0])) {
    return JNI_ERR;
  }
  (*env)->DeleteLocalRef(env, cls);
  return JNI_VERSION_1_6;
}

JNIEXPORT void JNICALL JNI_OnUnload(JavaVM *vm, void *reserved) {
  (void)reserved;
  JNIEnv *env;
  if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6)) {
    return;
  }
  jclass cls = (*env)->FindClass(env, "JniDomainProbe");
  if (!cls) {
    return;
  }
  jmethodID unloaded = (*env)->GetStaticMethodID(env, cls, "unloaded", "()V");
  if (unloaded) {
    (*env)->CallStaticVoidMethod(env, cls, unloaded);
  }
  (*env)->DeleteLocalRef(env, cls);
}
