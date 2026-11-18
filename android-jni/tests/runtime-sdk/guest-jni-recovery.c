#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
#include "jni-recovery.h"

static __thread sigjmp_buf jump_target;
static __thread ucontext_t context_target;

struct async_recovery {
  pthread_t recipient, sender;
  struct sdk_jni_witness *witness;
  struct sigaction previous_action;
  sigset_t previous_mask;
  int transfer;
};

static __thread struct async_recovery *signal_recovery;

static void receive_recovery_signal(int number, siginfo_t *info,
                                    void *context) {
  struct async_recovery *signal = signal_recovery;
  if (!signal || number != SIGUSR1 || !context || info->si_code != SI_QUEUE ||
      info->si_value.sival_int != 7139) {
    _exit(157);
  }

  atomic_store_explicit(&signal->witness->signal_delivered, 1,
                        memory_order_release);
  if (signal->transfer == SDK_JNI_ASYNC_JUMP) {
    siglongjmp(jump_target, 37);
  }
  setcontext(&context_target);
  _exit(158);
}

static void *send_recovery_signal(void *argument) {
  struct async_recovery *signal = argument;
  while (!atomic_load_explicit(&signal->witness->signal_requested,
                               memory_order_acquire)) {
    sched_yield();
  }
  assert(!pthread_sigqueue(signal->recipient, SIGUSR1,
                           (union sigval){.sival_int = 7139}));
  return NULL;
}

static void prepare_signal(struct async_recovery *state,
                           struct sdk_jni_witness *witness, int transfer) {
  assert(!signal_recovery);
  state->recipient = pthread_self();
  state->witness = witness;
  state->transfer = transfer;
  signal_recovery = state;

  struct sigaction action = {.sa_sigaction = receive_recovery_signal,
                             .sa_flags = SA_SIGINFO};
  sigemptyset(&action.sa_mask);
  sigset_t signals;
  sigemptyset(&signals);
  sigaddset(&signals, SIGUSR1);
  assert(!sigaction(SIGUSR1, &action, &state->previous_action));
  assert(!pthread_sigmask(SIG_UNBLOCK, &signals, &state->previous_mask));
  assert(!pthread_create(&state->sender, NULL, send_recovery_signal, state));
}

static void finish_signal(struct async_recovery *state) {
  assert(!pthread_join(state->sender, NULL));
  assert(atomic_load_explicit(&state->witness->signal_delivered,
                              memory_order_acquire) == 1);
  assert(!sigaction(SIGUSR1, &state->previous_action, NULL));
  assert(!pthread_sigmask(SIG_SETMASK, &state->previous_mask, NULL));
  signal_recovery = NULL;
}

static void use_reference(JNIEnv *env, jobject object, int resource) {
  if (resource == SDK_GLOBAL_REF) {
    jobject ref = (*env)->NewGlobalRef(env, object);
    assert(ref && (*env)->GetObjectRefType(env, ref) == JNIGlobalRefType);
    (*env)->DeleteGlobalRef(env, ref);
  } else if (resource == SDK_WEAK_REF) {
    jweak ref = (*env)->NewWeakGlobalRef(env, object);
    assert(ref && (*env)->GetObjectRefType(env, ref) == JNIWeakGlobalRefType);
    (*env)->DeleteWeakGlobalRef(env, ref);
  } else {
    jobject ref = (*env)->NewLocalRef(env, object);
    assert(ref && (*env)->GetObjectRefType(env, ref) == JNILocalRefType);
    (*env)->DeleteLocalRef(env, ref);
  }
}

static void use_string(JNIEnv *env, jstring string, int resource,
                       jboolean *copy) {
  if (resource == SDK_UTF_CHARS) {
    const char *chars = (*env)->GetStringUTFChars(env, string, copy);
    assert(chars && chars[0] == 'g');
    (*env)->ReleaseStringUTFChars(env, string, chars);
  } else if (resource == SDK_STRING_CRITICAL) {
    const jchar *chars = (*env)->GetStringCritical(env, string, copy);
    assert(chars && chars[0] == 'g');
    (*env)->ReleaseStringCritical(env, string, chars);
  } else {
    const jchar *chars = (*env)->GetStringChars(env, string, copy);
    assert(chars && chars[0] == 'g');
    (*env)->ReleaseStringChars(env, string, chars);
  }
}

// clang-format off
#define USE_ELEMENTS(Type, type)                                           \
  do {                                                                    \
    j##type##Array array = (j##type##Array)object;                         \
    j##type *elements = (*env)->Get##Type##ArrayElements(env,array,copy);   \
    assert(elements);                                                      \
    (*env)->Release##Type##ArrayElements(env,array,elements,JNI_ABORT);     \
  } while (0)
// clang-format on

static void use_resource(JNIEnv *env, jclass cls, jobject object, int resource,
                         jboolean *copy) {
  switch (resource) {
    case SDK_LOCAL_REF:
    case SDK_GLOBAL_REF:
    case SDK_WEAK_REF:
      use_reference(env, object, resource);
      break;
    case SDK_STRING_CHARS:
    case SDK_UTF_CHARS:
    case SDK_STRING_CRITICAL:
      use_string(env, object, resource, copy);
      break;
    case SDK_BOOLEAN_ELEMENTS:
      USE_ELEMENTS(Boolean, boolean);
      break;
    case SDK_BYTE_ELEMENTS:
      USE_ELEMENTS(Byte, byte);
      break;
    case SDK_CHAR_ELEMENTS:
      USE_ELEMENTS(Char, char);
      break;
    case SDK_SHORT_ELEMENTS:
      USE_ELEMENTS(Short, short);
      break;
    case SDK_INT_ELEMENTS:
      USE_ELEMENTS(Int, int);
      break;
    case SDK_LONG_ELEMENTS:
      USE_ELEMENTS(Long, long);
      break;
    case SDK_FLOAT_ELEMENTS:
      USE_ELEMENTS(Float, float);
      break;
    case SDK_DOUBLE_ELEMENTS:
      USE_ELEMENTS(Double, double);
      break;
    case SDK_ARRAY_CRITICAL: {
      void *elements = (*env)->GetPrimitiveArrayCritical(env, object, copy);
      assert(elements);
      (*env)->ReleasePrimitiveArrayCritical(env, object, elements, JNI_ABORT);
      break;
    }
    case SDK_LOCAL_FRAME: {
      assert((*env)->PushLocalFrame(env, 4) == JNI_OK);
      jobject promoted = (*env)->PopLocalFrame(env, object);
      assert(promoted);
      (*env)->DeleteLocalRef(env, promoted);
      break;
    }
    case SDK_MONITOR:
      assert((*env)->MonitorEnter(env, object) == JNI_OK);
      assert((*env)->MonitorExit(env, object) == JNI_OK);
      break;
    case SDK_METHOD_ID:
      assert((*env)->GetStaticMethodID(env, cls, "reply", "(I)I"));
      break;
    case SDK_FIELD_ID:
      assert((*env)->GetStaticFieldID(env, cls, "recoveryField", "I"));
      break;
    default:
      assert(!"Unknown JNI recovery fixture resource");
  }
}

#undef USE_ELEMENTS

jint sdk_try_guarded_context(JNIEnv *env, jclass cls) {
  (void)env;
  (void)cls;
  sigset_t original, observed;
  assert(!pthread_sigmask(SIG_SETMASK, NULL, &original));
  assert(setcontext(&context_target) == -1 && errno == ENOTSUP);
  assert(!pthread_sigmask(SIG_SETMASK, NULL, &observed));
  assert(sigismember(&original, SIGUSR1) == sigismember(&observed, SIGUSR1));
  return 73;
}

static jint managed_boundary(JNIEnv *env, jclass cls, jobject monitor) {
  jmethodID callback = (*env)->GetStaticMethodID(env, cls, "guardedRecovery",
                                                 "(Ljava/lang/Object;)I");
  assert(callback);
  volatile sig_atomic_t entered = 0;
  assert(!getcontext(&context_target) && !entered);

  entered = 1;
  jint result = (*env)->CallStaticIntMethod(env, cls, callback, monitor);
  assert(!(*env)->ExceptionCheck(env));
  return result;
}

static void check_retired_token(JNIEnv *env, struct sdk_jni_witness *witness) {
  if (witness->retired_token) {
    jobject token = (jobject)witness->retired_token;
    assert((*env)->GetObjectRefType(env, token) == JNIInvalidRefType);
    assert((*env)->ExceptionCheck(env));
    (*env)->ExceptionClear(env);
    witness->stale_token_rejected = 1;
  }
}

JNIEXPORT jint JNICALL sdk_jni_recovery(JNIEnv *env, jclass cls, jint target,
                                        jint resource, jobject object,
                                        jlong address) {
  struct sdk_jni_witness *witness = (void *)(uintptr_t)address;
  if (target == SDK_JNI_JUMP_SOURCE) {
    siglongjmp(jump_target, 37);
  }
  if (target == SDK_JNI_CONTEXT_SOURCE) {
    return setcontext(&context_target) == -1 ? -errno : -1;
  }
  if (target == SDK_JNI_GUARD) {
    return managed_boundary(env, cls, object);
  }
  if (target == SDK_JNI_NORMAL || target == SDK_JNI_NESTED ||
      target == SDK_JNI_TO_HOST) {
    witness->armed = 1;
    use_resource(env, cls, object, resource, &witness->copy);
    witness->armed = 0;
    return target == SDK_JNI_NORMAL ? 73 : -2;
  }

  jobject survivor = (*env)->NewLocalRef(env, object);
  assert(survivor);
  int asynchronous =
      target == SDK_JNI_ASYNC_JUMP || target == SDK_JNI_ASYNC_CONTEXT;
  struct async_recovery signal;
  if (asynchronous) {
    prepare_signal(&signal, witness, target);
  }

  if (target == SDK_JNI_TO_GUEST_JUMP || target == SDK_JNI_ASYNC_JUMP) {
    int value = sigsetjmp(jump_target, 1);
    if (!value) {
      witness->armed = 1;
      use_resource(env, cls, object, resource, &witness->copy);
      return -3;
    }
    assert(value == 37);
  } else {
    assert(target == SDK_JNI_TO_GUEST_CONTEXT ||
           target == SDK_JNI_ASYNC_CONTEXT);
    volatile sig_atomic_t entered = 0;
    assert(!getcontext(&context_target));
    if (!entered) {
      entered = 1;
      witness->armed = 1;
      use_resource(env, cls, object, resource, &witness->copy);
      return -4;
    }
  }

  witness->armed = 0;
  if (asynchronous) {
    finish_signal(&signal);
  }
  assert(witness->copy == SDK_COPY_SENTINEL);
  assert(!(*env)->ExceptionCheck(env));
  assert((*env)->GetObjectRefType(env, survivor) == JNILocalRefType);
  assert((*env)->IsSameObject(env, survivor, object));
  (*env)->DeleteLocalRef(env, survivor);
  check_retired_token(env, witness);
  return (*env)->GetVersion(env) >= JNI_VERSION_1_6 ? 73 : -5;
}
