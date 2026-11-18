#define _GNU_SOURCE
#include <assert.h>
#include <stddef.h>
#include <sched.h>
#include <stdio.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "loader_backend.h"
#include "internal/domain_unwind.h"
#include "jni-recovery.h"
#include "nonlocal_probe.h"

#ifdef GW_TEST_NATIVE_UNWIND
#define API(name) \
  ((int)(offsetof(struct JNINativeInterface, name) / sizeof(void *)) - 4)

static const struct {
  int api;
  const char *name;
} resources[] = {
    [SDK_LOCAL_REF] = {API(NewLocalRef), "NewLocalRef"},
    [SDK_GLOBAL_REF] = {API(NewGlobalRef), "NewGlobalRef"},
    [SDK_WEAK_REF] = {API(NewWeakGlobalRef), "NewWeakGlobalRef"},
    [SDK_STRING_CHARS] = {API(GetStringChars), "GetStringChars"},
    [SDK_UTF_CHARS] = {API(GetStringUTFChars), "GetStringUTFChars"},
    [SDK_BOOLEAN_ELEMENTS] = {API(GetBooleanArrayElements),
                              "GetBooleanArrayElements"},
    [SDK_BYTE_ELEMENTS] = {API(GetByteArrayElements), "GetByteArrayElements"},
    [SDK_CHAR_ELEMENTS] = {API(GetCharArrayElements), "GetCharArrayElements"},
    [SDK_SHORT_ELEMENTS] = {API(GetShortArrayElements),
                            "GetShortArrayElements"},
    [SDK_INT_ELEMENTS] = {API(GetIntArrayElements), "GetIntArrayElements"},
    [SDK_LONG_ELEMENTS] = {API(GetLongArrayElements), "GetLongArrayElements"},
    [SDK_FLOAT_ELEMENTS] = {API(GetFloatArrayElements),
                            "GetFloatArrayElements"},
    [SDK_DOUBLE_ELEMENTS] = {API(GetDoubleArrayElements),
                             "GetDoubleArrayElements"},
    [SDK_STRING_CRITICAL] = {API(GetStringCritical), "GetStringCritical"},
    [SDK_ARRAY_CRITICAL] = {API(GetPrimitiveArrayCritical),
                            "GetPrimitiveArrayCritical"},
    [SDK_LOCAL_FRAME] = {API(PushLocalFrame), "PushLocalFrame"},
    [SDK_MONITOR] = {API(MonitorEnter), "MonitorEnter"},
    [SDK_METHOD_ID] = {API(GetStaticMethodID), "GetStaticMethodID"},
    [SDK_FIELD_ID] = {API(GetStaticFieldID), "GetStaticFieldID"},
};

#undef API

typedef jint (*entry_fn)(JNIEnv *, jclass, jint, jint, jobject, jlong);

struct recovery_fixture {
  JNIEnv *env;
  jclass cls, thread_class;
  jmethodID holds_lock;
  entry_fn entry;
  jobject objects[SDK_FIELD_ID + 1];
  jthrowable throwable;
};

enum { MAX_CASE_DEPTH = 3 };

struct recovery_case {
  struct recovery_fixture *fixture;
  struct sdk_jni_witness witness;
  sigjmp_buf escape;
  int target, count, acquired, discarded;
  int throw_exception;
  int sequence[MAX_CASE_DEPTH];
  uintptr_t values[MAX_CASE_DEPTH], tokens[MAX_CASE_DEPTH];
};

static _Thread_local struct recovery_case *active_case;

static jint invoke(struct recovery_case *test, int target, int resource) {
  struct recovery_fixture *fixture = test->fixture;
  return fixture->entry(fixture->env, fixture->cls, target, resource,
                        fixture->objects[resource],
                        (jlong)(uintptr_t)&test->witness);
}

static void escape_host(int number) {
  assert(number == SDK_HOST_ESCAPE_SIGNAL && active_case);
  siglongjmp(active_case->escape, 37);
}

static void transfer_from_result(struct recovery_case *test) {
  if (test->acquired < test->count) {
    invoke(test, SDK_JNI_NESTED, test->sequence[test->acquired]);
  } else if (test->target == SDK_JNI_TO_HOST) {
    if (test->throw_exception) {
      JNIEnv *env = test->fixture->env;
      assert((*env)->Throw(env, test->fixture->throwable) == JNI_OK);
    }
    assert(!syscall(SYS_tgkill, getpid(), syscall(SYS_gettid),
                    SDK_HOST_ESCAPE_SIGNAL));
  } else if (test->target == SDK_JNI_ASYNC_JUMP ||
             test->target == SDK_JNI_ASYNC_CONTEXT) {
    atomic_store_explicit(&test->witness.signal_requested, 1,
                          memory_order_release);
    for (;;) {
      sched_yield();
    }
  } else {
    int source = test->target == SDK_JNI_TO_GUEST_JUMP ? SDK_JNI_JUMP_SOURCE
                                                       : SDK_JNI_CONTEXT_SOURCE;
    jint result = invoke(test, source, test->sequence[0]);
    fprintf(stderr, "Guest transfer source returned %d\n", result);
  }
  assert(!"JNI callback returned after its result was abandoned");
}

void gw_domain_nonlocal_test_result(int point, int api, uintptr_t value,
                                    uintptr_t token) {
  struct recovery_case *test = active_case;
  if (!test || !test->witness.armed) {
    return;
  }

  if (point == GW_UNWIND_RPC_RESULT_DISCARDED) {
    int index = test->acquired - ++test->discarded;
    assert(index >= 0 && api == resources[test->sequence[index]].api);
    assert(value == test->values[index] && token == test->tokens[index]);
    if (token) {
      test->witness.retired_token = token;
    }
    return;
  }

  assert(point == GW_UNWIND_RPC_RESULT_OWNED && test->acquired < test->count);
  int index = test->acquired++;
  assert(api == resources[test->sequence[index]].api);
  test->values[index] = value;
  test->tokens[index] = token;
  if (test->target == SDK_JNI_NORMAL) {
    return;
  }
  if (test->sequence[index] == SDK_BYTE_ELEMENTS) {
    assert(value);
    ((jbyte *)value)[0] = 97; /* A discarded copy must never reach Java. */
  }
  transfer_from_result(test);
}

static void run_case(struct recovery_case *test) {
  assert(!active_case && test->count > 0 && test->count <= MAX_CASE_DEPTH);
  active_case = test;
  test->witness.copy = SDK_COPY_SENTINEL;
  if (test->target == SDK_JNI_TO_HOST) {
    int landed = sigsetjmp(test->escape, 1);
    if (!landed) {
      invoke(test, test->target, test->sequence[0]);
      assert(!"Host did not recover from JNI result window");
    }
    assert(landed == 37);
  } else {
    assert(invoke(test, test->target, test->sequence[0]) == 73);
  }
  active_case = NULL;

  assert(test->acquired == test->count);
  if (test->target == SDK_JNI_NORMAL) {
    assert(!test->discarded && test->witness.copy == JNI_TRUE);
  } else {
    assert(test->discarded == test->count);
    assert(test->witness.copy == SDK_COPY_SENTINEL);
  }

  JNIEnv *env = test->fixture->env;
  if (test->throw_exception) {
    jthrowable observed = (*env)->ExceptionOccurred(env);
    assert(observed);
    (*env)->ExceptionClear(env);
    assert((*env)->IsSameObject(env, observed, test->fixture->throwable));
    (*env)->DeleteLocalRef(env, observed);
  }
  assert(!(*env)->ExceptionCheck(env));
  jbyte first = 0;
  (*env)->GetByteArrayRegion(env, test->fixture->objects[SDK_BYTE_ELEMENTS], 0,
                             1, &first);
  assert(!(*env)->ExceptionCheck(env) && first == 11);
}

static struct recovery_fixture prepare_fixture(JNIEnv *env, jclass cls,
                                               entry_fn entry) {
  struct recovery_fixture fixture = {.env = env, .cls = cls, .entry = entry};
  assert((*env)->PushLocalFrame(env, 32) == JNI_OK);
  jstring text = (*env)->NewStringUTF(env, "glibc-worker");
  assert(text);
  for (int i = SDK_LOCAL_REF; i <= SDK_FIELD_ID; ++i) {
    fixture.objects[i] = text;
  }
  fixture.objects[SDK_BOOLEAN_ELEMENTS] = (*env)->NewBooleanArray(env, 2);
  fixture.objects[SDK_BYTE_ELEMENTS] = (*env)->NewByteArray(env, 1024 * 1024);
  fixture.objects[SDK_CHAR_ELEMENTS] = (*env)->NewCharArray(env, 2);
  fixture.objects[SDK_SHORT_ELEMENTS] = (*env)->NewShortArray(env, 2);
  fixture.objects[SDK_INT_ELEMENTS] = (*env)->NewIntArray(env, 2);
  fixture.objects[SDK_LONG_ELEMENTS] = (*env)->NewLongArray(env, 2);
  fixture.objects[SDK_FLOAT_ELEMENTS] = (*env)->NewFloatArray(env, 2);
  fixture.objects[SDK_DOUBLE_ELEMENTS] = (*env)->NewDoubleArray(env, 2);
  fixture.objects[SDK_ARRAY_CRITICAL] = fixture.objects[SDK_INT_ELEMENTS];
  for (int i = 0; i <= SDK_FIELD_ID; ++i) {
    assert(fixture.objects[i]);
  }

  jbyte first = 11;
  (*env)->SetByteArrayRegion(env, fixture.objects[SDK_BYTE_ELEMENTS], 0, 1,
                             &first);
  assert(!(*env)->ExceptionCheck(env));
  jclass throwable = (*env)->FindClass(env, "java/lang/IllegalStateException");
  assert(throwable);
  jmethodID init = (*env)->GetMethodID(env, throwable, "<init>", "()V");
  assert(init);
  fixture.throwable = (*env)->NewObject(env, throwable, init);
  assert(fixture.throwable && !(*env)->ExceptionCheck(env));
  fixture.thread_class = (*env)->FindClass(env, "java/lang/Thread");
  assert(fixture.thread_class);
  fixture.holds_lock = (*env)->GetStaticMethodID(
      env, fixture.thread_class, "holdsLock", "(Ljava/lang/Object;)Z");
  assert(fixture.holds_lock);
  return fixture;
}

static int holds_monitor(struct recovery_fixture *fixture) {
  JNIEnv *env = fixture->env;
  jboolean owns = (*env)->CallStaticBooleanMethod(
      env, fixture->thread_class, fixture->holds_lock,
      fixture->objects[SDK_MONITOR]);
  assert(!(*env)->ExceptionCheck(env));
  return owns;
}

static void recover_to_host(struct recovery_fixture *fixture) {
  JNIEnv *env = fixture->env;
  for (int resource = SDK_LOCAL_REF; resource <= SDK_FIELD_ID; ++resource) {
    struct recovery_case test = {.fixture = fixture,
                                 .target = SDK_JNI_TO_HOST,
                                 .count = 1,
                                 .sequence = {resource},
                                 .throw_exception = resource == SDK_UTF_CHARS};
    if (resource == SDK_MONITOR) {
      assert((*env)->MonitorEnter(env, fixture->objects[resource]) == JNI_OK);
    }
    run_case(&test);
    if (resource == SDK_MONITOR) {
      assert(holds_monitor(fixture));
      assert((*env)->MonitorExit(env, fixture->objects[resource]) == JNI_OK);
      assert(!holds_monitor(fixture));
    }
    if (resource == SDK_BYTE_ELEMENTS) {
      struct recovery_case control = {.fixture = fixture,
                                      .target = SDK_JNI_NORMAL,
                                      .count = 1,
                                      .sequence = {resource}};
      run_case(&control);
      assert(control.values[0] == test.values[0]);
    }
    printf(
        "{\"stage\":\"jni-resource-host\",\"api\":\"%s\",\"discarded\":1,"
        "\"pending_preserved\":%s}\n",
        resources[resource].name, test.throw_exception ? "true" : "false");
  }
}

static void recover_to_guest(struct recovery_fixture *fixture) {
  struct recovery_case plans[] = {
      {.fixture = fixture,
       .target = SDK_JNI_TO_GUEST_JUMP,
       .count = 3,
       .sequence = {SDK_GLOBAL_REF, SDK_BYTE_ELEMENTS, SDK_UTF_CHARS}},
      {.fixture = fixture,
       .target = SDK_JNI_TO_GUEST_CONTEXT,
       .count = 3,
       .sequence = {SDK_WEAK_REF, SDK_LOCAL_FRAME, SDK_METHOD_ID}},
  };
  for (size_t i = 0; i < sizeof plans / sizeof *plans; ++i) {
    struct recovery_case *test = &plans[i];
    run_case(test);
    assert(test->witness.stale_token_rejected);
    printf(
        "{\"stage\":\"jni-resource-guest\",\"transfer\":\"%s\","
        "\"discarded\":%d,\"lifo\":true,\"stale_token_rejected\":true,"
        "\"copyout_unchanged\":true,\"reentered_jni\":true}\n",
        test->target == SDK_JNI_TO_GUEST_JUMP ? "siglongjmp" : "setcontext",
        test->discarded);
  }
}

static struct recovery_fixture open_fixture(
    JNIEnv *env, jclass cls, const struct gw_loader_api_v1 *loader,
    uint64_t module) {
  struct gw_loader_error error = {0};
  void *symbol = NULL;
  assert(loader->find(env, module, "sdk_jni_recovery",
                      "(IILjava/lang/Object;J)I", &symbol, &error) == GW_OK);
  return prepare_fixture(env, cls, (entry_fn)symbol);
}

void sdk_run_jni_recovery(JNIEnv *env, jclass cls,
                          const struct gw_loader_api_v1 *loader,
                          uint64_t module) {
  struct recovery_fixture fixture = open_fixture(env, cls, loader, module);
  struct sigaction action = {.sa_handler = escape_host}, previous;
  sigemptyset(&action.sa_mask);
  assert(!sigaction(SDK_HOST_ESCAPE_SIGNAL, &action, &previous));

  recover_to_host(&fixture);
  recover_to_guest(&fixture);
  assert(!sigaction(SDK_HOST_ESCAPE_SIGNAL, &previous, NULL));

  struct recovery_case guard = {.fixture = &fixture};
  assert(invoke(&guard, SDK_JNI_GUARD, SDK_MONITOR) == 73);
  jfieldID completed = (*env)->GetStaticFieldID(env, cls, "guardFinally", "I");
  assert(completed && (*env)->GetStaticIntField(env, cls, completed) == 1);
  assert(!holds_monitor(&fixture));
  puts(
      "{\"stage\":\"jni-managed-boundary\",\"rejected\":true,"
      "\"monitor_released\":true,\"finally_ran\":true}");
  (*env)->PopLocalFrame(env, NULL);
}

void sdk_run_jni_recovery_async(JNIEnv *env, jclass cls,
                                const struct gw_loader_api_v1 *loader,
                                uint64_t module) {
  struct recovery_fixture fixture = open_fixture(env, cls, loader, module);
  struct recovery_case plans[] = {
      {.fixture = &fixture,
       .target = SDK_JNI_ASYNC_JUMP,
       .count = 1,
       .sequence = {SDK_ARRAY_CRITICAL}},
      {.fixture = &fixture,
       .target = SDK_JNI_ASYNC_CONTEXT,
       .count = 1,
       .sequence = {SDK_STRING_CRITICAL}},
      {.fixture = &fixture,
       .target = SDK_JNI_ASYNC_CONTEXT,
       .count = 1,
       .sequence = {SDK_GLOBAL_REF}},
  };

  for (size_t i = 0; i < sizeof plans / sizeof *plans; ++i) {
    struct recovery_case *test = &plans[i];
    run_case(test);
    int resource = test->sequence[0];
    assert(test->witness.signal_delivered &&
           test->witness.stale_token_rejected == (resource == SDK_GLOBAL_REF));
    printf(
        "{\"stage\":\"jni-resource-signal\",\"api\":\"%s\","
        "\"transfer\":\"%s\",\"discarded\":1,\"same_carrier\":true}\n",
        resources[resource].name,
        test->target == SDK_JNI_ASYNC_JUMP ? "siglongjmp" : "setcontext");
  }
  (*env)->PopLocalFrame(env, NULL);
}
#endif
