#define _GNU_SOURCE
#include <jni.h>
#include <jvmti.h>
#include <assert.h>
#include <dlfcn.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

/* This disposable reproducer deliberately crosses VM frames using only the
 * native libc. It is not a supported JNI implementation or recovery adapter.
 * JavaCallWrapper normally restores anchors, handles and thread transitions:
 * https://github.com/openjdk/jdk17u/blob/master/src/hotspot/share/runtime/javaCalls.cpp
 * JNI exception propagation returns through the VM instead:
 * https://docs.oracle.com/en/java/javase/17/docs/specs/jni/design.html#exceptions-and-error-codes
 */
static JavaVM *vm;
static jvmtiEnv *ti;
static jclass probe;
static jmethodID contend;
static const char *mode;
static _Thread_local sigjmp_buf target;
static _Thread_local ucontext_t context;
static _Thread_local volatile sig_atomic_t resumed;
static _Atomic int entered, finalized, acquired;
static _Atomic int ready, start, attempted, waiter_done, observer_done;
static _Atomic int waiter_state;
static jthread waiter_thread;

static void pause_briefly(void) {
  struct timespec delay = {.tv_nsec = 1000000};
  nanosleep(&delay, NULL);
}

static uint64_t clock_ns(void) {
  struct timespec now;
  assert(!clock_gettime(CLOCK_MONOTONIC, &now));
  return (uint64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}

static void JNICALL escape_native(JNIEnv *env, jclass cls) {
  (void)cls;
  atomic_store(&entered, 1);
  if (!strcmp(mode, "jump")) {
    siglongjmp(target, 37);
  }
  if (!strcmp(mode, "context")) {
    resumed = 1;
    assert(!setcontext(&context));
    abort();
  }
  if (!strcmp(mode, "exception")) {
    jclass exception =
        (*env)->FindClass(env, "java/lang/IllegalStateException");
    assert(exception);
    assert(!(*env)->ThrowNew(env, exception, "native recovery control"));
    (*env)->DeleteLocalRef(env, exception);
  }
}

static void JNICALL mark_native(JNIEnv *env, jclass cls, jint event) {
  (void)env;
  (void)cls;
  if (event == 2) {
    atomic_fetch_add(&finalized, 1);
  } else if (event == 3) {
    atomic_fetch_add(&acquired, 1);
  } else {
    abort();
  }
}

static void *waiter(void *unused) {
  (void)unused;
  JNIEnv *env;
  assert((*vm)->AttachCurrentThread(vm, (void **)&env, NULL) == JNI_OK);
  jthread thread;
  assert((*ti)->GetCurrentThread(ti, &thread) == JVMTI_ERROR_NONE);
  waiter_thread = (*env)->NewGlobalRef(env, thread);
  assert(waiter_thread);
  (*env)->DeleteLocalRef(env, thread);
  atomic_fetch_or_explicit(&ready, 1, memory_order_release);
  while (!atomic_load_explicit(&start, memory_order_acquire)) {
    pause_briefly();
  }
  atomic_store(&attempted, 1);
  (*env)->CallStaticVoidMethod(env, probe, contend);
  assert(!(*env)->ExceptionCheck(env));
  atomic_store(&waiter_done, 1);
  assert((*vm)->DetachCurrentThread(vm) == JNI_OK);
  return NULL;
}

static void *observer(void *unused) {
  (void)unused;
  JNIEnv *env;
  assert((*vm)->AttachCurrentThread(vm, (void **)&env, NULL) == JNI_OK);
  atomic_fetch_or_explicit(&ready, 2, memory_order_release);
  while (!atomic_load_explicit(&start, memory_order_acquire)) {
    pause_briefly();
  }
  uint64_t deadline = clock_ns() + 3000000000;
  for (;;) {
    jint state = 0;
    assert((*ti)->GetThreadState(ti, waiter_thread, &state) ==
           JVMTI_ERROR_NONE);
    atomic_store(&waiter_state, state);
    if (atomic_load(&waiter_done) ||
        (state & JVMTI_THREAD_STATE_BLOCKED_ON_MONITOR_ENTER)) {
      break;
    }
    assert(clock_ns() < deadline);
    pause_briefly();
  }
  assert((*vm)->DetachCurrentThread(vm) == JNI_OK);
  atomic_store_explicit(&observer_done, 1, memory_order_release);
  return NULL;
}

int main(int argc, char **argv) {
  assert(argc == 5);
  mode = argv[3];
  assert(!strcmp(mode, "normal") || !strcmp(mode, "exception") ||
         !strcmp(mode, "jump") || !strcmp(mode, "context"));
  void *library = dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL);
  assert(library);
  jint (*create)(JavaVM **, void **, void *) =
      dlsym(library, "JNI_CreateJavaVM");
  assert(create);
  char *classpath;
  assert(asprintf(&classpath, "-Djava.class.path=%s", argv[2]) > 0);
  JavaVMOption options[] = {{.optionString = classpath},
                            {.optionString = "-Xcheck:jni"},
                            {.optionString = "-Xint"}};
  JavaVMInitArgs args = {.version = JNI_VERSION_1_6,
                         .nOptions = !strcmp(argv[4], "int") ? 3 : 2,
                         .options = options};
  JNIEnv *env;
  assert(create(&vm, (void **)&env, &args) == JNI_OK);
  free(classpath);
  assert((*vm)->GetEnv(vm, (void **)&ti, JVMTI_VERSION_1_2) == JNI_OK);
  jclass local = (*env)->FindClass(env, "ManagedBoundary");
  assert(local);
  probe = (*env)->NewGlobalRef(env, local);
  (*env)->DeleteLocalRef(env, local);
  JNINativeMethod methods[] = {{"escape", "()V", (void *)escape_native},
                               {"mark", "(I)V", (void *)mark_native}};
  assert(!(*env)->RegisterNatives(env, probe, methods, 2));
  jmethodID enter = (*env)->GetStaticMethodID(env, probe, "enter", "()V");
  contend = (*env)->GetStaticMethodID(env, probe, "contend", "()V");
  assert(enter && contend && !(*env)->ExceptionCheck(env));
  pthread_t waiter_id, observer_id;
  assert(!pthread_create(&waiter_id, NULL, waiter, NULL));
  assert(!pthread_create(&observer_id, NULL, observer, NULL));
  while (atomic_load_explicit(&ready, memory_order_acquire) != 3) {
    pause_briefly();
  }
  if (!strcmp(mode, "context")) {
    assert(!getcontext(&context));
    if (!resumed) {
      (*env)->CallStaticVoidMethod(env, probe, enter);
      abort();
    }
  } else if (!sigsetjmp(target, 1)) {
    (*env)->CallStaticVoidMethod(env, probe, enter);
    if ((*env)->ExceptionCheck(env)) {
      assert(!strcmp(mode, "exception"));
      (*env)->ExceptionClear(env);
    }
  }
  assert(atomic_load(&entered));
  /* No JNI calls on the abandoned carrier: observer owns its own real env. */
  atomic_store_explicit(&start, 1, memory_order_release);
  while (!atomic_load_explicit(&observer_done, memory_order_acquire)) {
    pause_briefly();
  }
  assert(!pthread_join(observer_id, NULL));
  int unsafe = !strcmp(mode, "jump") || !strcmp(mode, "context");
  if (!unsafe) {
    assert(!pthread_join(waiter_id, NULL));
    (*env)->DeleteGlobalRef(env, waiter_thread);
    (*env)->DeleteGlobalRef(env, probe);
    assert((*vm)->DestroyJavaVM(vm) == JNI_OK);
  }
  printf(
      "{\"mode\":\"%s\",\"engine\":\"%s\",\"bridge_present\":false,"
      "\"native_target_reached\":true,\"finally_count\":%d,"
      "\"monitor_acquired\":%s,\"waiter_attempted\":%s,"
      "\"monitor_blocked\":%s,\"vm_destroyed\":%s}\n",
      mode, argv[4], atomic_load(&finalized),
      atomic_load(&acquired) ? "true" : "false",
      atomic_load(&attempted) ? "true" : "false",
      atomic_load(&waiter_state) & JVMTI_THREAD_STATE_BLOCKED_ON_MONITOR_ENTER
          ? "true"
          : "false",
      unsafe ? "false" : "true");
  fflush(stdout);
  if (unsafe) {
    _exit(0); /* The unreconciled VM and blocked waiter are process-disposed. */
  }
}
