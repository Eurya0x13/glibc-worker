#define _GNU_SOURCE
#include "host-hooks.h"
#include "vm-start.h"
#include <jni.h>
#include <dlfcn.h>
#include <errno.h>
#include <fenv.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __ANDROID__
#include "jre_layout_spike.h"
#include "jre-layout.h"
#endif

static JavaVM *vm;
static JNIEnv *main_env;
static jclass probe_class;
static jmethodID callback_method;
static _Thread_local JNIEnv *thread_env;
static _Thread_local struct exchange *active_exchange;
static _Thread_local int (*active_invoke)(struct exchange *);
static _Atomic int attachments, detachments, callbacks, reentries;
extern _Atomic int domain_forwarded_faults;
extern _Atomic int domain_guest_host_signals;
extern _Atomic int domain_expected_host_signals;

static int exception(JNIEnv *env) {
  if (!(*env)->ExceptionCheck(env)) {
    return 0;
  }
  (*env)->ExceptionDescribe(env);
  (*env)->ExceptionClear(env);
  return 1;
}

static jint JNICALL reenter(JNIEnv *env, jclass cls) {
  (void)cls;
  struct exchange *e = active_exchange;
  if (!e || env != thread_env || e->depth + 1 >= DOMAIN_NESTING) {
    return -1;
  }
  int saved_errno = errno;
  fenv_t saved_fp;
  fegetenv(&saved_fp);
  fesetround(FE_DOWNWARD);
  errno = EDOM;
  ++e->depth;
  int result = active_invoke(e);
  --e->depth;
  ++e->nested;
  atomic_fetch_add(&reentries, 1);
  fesetenv(&saved_fp);
  errno = saved_errno;
  return result;
}

int experiment_start(void) {
  if (domain_vm_start(&vm, &main_env, 1)) {
    return 1;
  }
  jclass local = (*main_env)->FindClass(main_env, "DomainProbe");
  if (exception(main_env) || !local) {
    return 1;
  }
  probe_class = (*main_env)->NewGlobalRef(main_env, local);
  if (exception(main_env) || !probe_class) {
    return 1;
  }
  (*main_env)->DeleteLocalRef(main_env, local);
  JNINativeMethod method = {
      .name = "reenter", .signature = "()I", .fnPtr = (void *)reenter};
  if (!probe_class ||
      (*main_env)->RegisterNatives(main_env, probe_class, &method, 1) ||
      exception(main_env)) {
    return 1;
  }
  callback_method = (*main_env)->GetStaticMethodID(main_env, probe_class,
                                                   "callback", "(II)I");
  if (exception(main_env) || !callback_method) {
    return 1;
  }
  jmethodID start =
      (*main_env)->GetStaticMethodID(main_env, probe_class, "start", "()V");
  if (exception(main_env) || !start) {
    return 1;
  }
  (*main_env)->CallStaticVoidMethod(main_env, probe_class, start);
  return exception(main_env);
}

int experiment_thread_enter(void) {
  if ((*vm)->AttachCurrentThread(vm, (void **)&thread_env, NULL) != JNI_OK) {
    return 1;
  }
  atomic_fetch_add(&attachments, 1);
  return 0;
}

int experiment_callback(struct exchange *e, int value,
                        int (*invoke)(struct exchange *)) {
  int saved_errno = errno;
  fenv_t saved_fp;
  fegetenv(&saved_fp);
  fesetround(FE_TONEAREST);
  struct exchange *previous_exchange = active_exchange;
  int (*previous_invoke)(struct exchange *) = active_invoke;
  active_exchange = e;
  active_invoke = invoke;
  jint result = (*thread_env)
                    ->CallStaticIntMethod(thread_env, probe_class,
                                          callback_method, value, e->depth);
  CHECK(e, !exception(thread_env));
  active_exchange = previous_exchange;
  active_invoke = previous_invoke;
  atomic_fetch_add(&callbacks, 1);
  fesetenv(&saved_fp);
  errno = saved_errno;
  return result;
}

int experiment_thread_leave(void) {
  int result = (*vm)->DetachCurrentThread(vm);
  thread_env = NULL;
  if (result == JNI_OK) {
    atomic_fetch_add(&detachments, 1);
  }
  return result != JNI_OK;
}

int experiment_finish(void) {
  jmethodID finish =
      (*main_env)->GetStaticMethodID(main_env, probe_class, "finish", "()J");
  if (exception(main_env) || !finish) {
    return 1;
  }
  jlong collections =
      (*main_env)->CallStaticLongMethod(main_env, probe_class, finish);
  int failed = exception(main_env);
  failed |= (*main_env)->UnregisterNatives(main_env, probe_class) != JNI_OK;
  (*main_env)->DeleteGlobalRef(main_env, probe_class);
  int destroyed = (*vm)->DestroyJavaVM(vm);
  int attached = atomic_load(&attachments),
      detached = atomic_load(&detachments);
  int called = atomic_load(&callbacks), nested = atomic_load(&reentries);
  failed |= destroyed != JNI_OK || attached != 24 || detached != 24 ||
            called != 19200 || nested != 16800 || collections <= 0 ||
            atomic_load(&domain_expected_host_signals) < 24 ||
            atomic_load(&domain_guest_host_signals) <
                atomic_load(&domain_expected_host_signals);
  printf(
      "HOTSPOT_RESULT {\"result\":\"%s\",\"attached\":%d,\"detached\":%d,"
      "\"java_callbacks\":%d,\"java_native_reentries\":%d,\"gc_cycles\":%lld,"
      "\"forwarded_faults\":%d,\"guest_host_signals\":%d,\"expected_host_"
      "signals\":%d,\"destroy_status\":%d}"
      "\n",
      failed ? "FAIL" : "PASS", attached, detached, called, nested,
      (long long)collections, atomic_load(&domain_forwarded_faults),
      atomic_load(&domain_guest_host_signals),
      atomic_load(&domain_expected_host_signals), destroyed);
  return !!failed;
}
