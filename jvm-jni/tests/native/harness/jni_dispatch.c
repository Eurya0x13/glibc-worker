#include <jni.h>
#include "protocol.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

/* Only the HotSpot-attached bionic Host thread touches these JNI objects.
   Tokens are invocation-scoped and monotonically allocated, so deleted tokens
   stay stale. */
struct handle {
  int kind;
  jclass cls;
  jmethodID method;
  uintptr_t parent;
};
static struct handle handles[16];
static unsigned next_handle = 1;
static JNIEnv *real_env;
static JavaVM *real_vm;
static int real_tid, errors, calls, exceptions, checked_requests;

JNIEnv *app_host_jni_env(void) {
  return real_env;
}

int app_host_jni_begin(JNIEnv *env) {
  real_env = env;
  real_tid = (int)syscall(SYS_gettid);
  return (*env)->GetJavaVM(env, &real_vm);
}

static uintptr_t add_handle(struct handle h) {
  if (next_handle >= sizeof handles / sizeof handles[0]) {
    return 0;
  }
  uintptr_t token = next_handle++;
  handles[token] = h;
  return token;
}

static struct handle *lookup(uintptr_t token, int kind) {
  if (!token || token >= next_handle || handles[token].kind != kind) {
    return NULL;
  }
  return &handles[token];
}

void app_host_jni_dispatch(struct shared *s) {
  int saved_errno = errno;
  struct jni_rpc *r = &s->jni;
  int sequence = atomic_load_explicit(&r->request, memory_order_acquire);
  if (sequence == atomic_load_explicit(&r->response, memory_order_relaxed)) {
    return;
  }
  s->jni_requests++;

  JNIEnv *current = NULL;
  if ((int)syscall(SYS_gettid) != real_tid ||
      (*real_vm)->GetEnv(real_vm, (void **)&current, JNI_VERSION_1_6) !=
          JNI_OK ||
      current != real_env) {
    r->error = 1;
    goto done;
  }
  checked_requests++;

  JNIEnv *env = real_env;
  if ((*env)->ExceptionCheck(env) && r->op != J_EXCEPTION_CHECK &&
      r->op != J_EXCEPTION_CLEAR && r->op != J_DELETE_LOCAL) {
    r->error = 2;
    goto done;
  }

  struct handle *cls = NULL, *method = NULL;
  switch (r->op) {
    case J_VERSION:
      r->result = (*env)->GetVersion(env);
      break;
    case J_FIND_CLASS: {
      if (!memchr(r->name, 0, sizeof r->name)) {
        r->error = 3;
        break;
      }
      jclass ref = (*env)->FindClass(env, r->name);
      if (ref) {
        r->result = (int64_t)add_handle((struct handle){.kind = 1, .cls = ref});
        if (!r->result) {
          (*env)->DeleteLocalRef(env, ref);
          r->error = 4;
        }
      }
      break;
    }

    case J_STATIC_METHOD:
      cls = lookup(r->object, 1);
      if (!cls || !memchr(r->name, 0, sizeof r->name) ||
          !memchr(r->signature, 0, sizeof r->signature) ||
          strcmp(r->signature, "(I)I")) {
        r->error = 5;
        break;
      }
      {
        jmethodID id =
            (*env)->GetStaticMethodID(env, cls->cls, r->name, r->signature);
        if (id) {
          r->result = (int64_t)add_handle(
              (struct handle){.kind = 2, .method = id, .parent = r->object});
          if (!r->result) {
            r->error = 6;
          }
        }
      }
      break;

    case J_CALL_INT:
      cls = lookup(r->object, 1);
      method = lookup(r->method, 2);
      if (!cls || !method || method->parent != r->object) {
        r->error = 7;
        break;
      }
      {
        jvalue value = {.i = r->arg};
        r->result =
            (*env)->CallStaticIntMethodA(env, cls->cls, method->method, &value);
        if ((*env)->ExceptionCheck(env)) {
          exceptions++;
          r->result = 0;
        } else {
          calls++;
        }
      }
      break;

    case J_EXCEPTION_CHECK:
      r->result = (*env)->ExceptionCheck(env);
      break;
    case J_EXCEPTION_CLEAR:
      (*env)->ExceptionClear(env);
      break;
    case J_DELETE_LOCAL:
      if (!r->object) {
        break;
      }
      cls = lookup(r->object, 1);
      if (!cls) {
        r->error = 8;
        break;
      }
      (*env)->DeleteLocalRef(env, cls->cls);
      cls->kind = 0;
      break;
    default:
      r->error = 9;
      break;
  }

done:
  if (r->error) {
    errors++;
  }
  printf("HOST_JNI_RPC sequence=%d op=%d tid=%d error=%d\n", sequence, r->op,
         real_tid, r->error);
  errno = saved_errno;
  atomic_store_explicit(&r->response, sequence, memory_order_release);
}

int app_host_jni_finish(void) {
  int pending = (*real_env)->ExceptionCheck(real_env);
  if (pending) {
    (*real_env)->ExceptionDescribe(real_env);
    (*real_env)->ExceptionClear(real_env);
  }

  int live = 0;
  for (unsigned i = 1; i < next_handle; i++) {
    if (handles[i].kind == 1) {
      live++;
    }
  }

  int good = !errors && !pending && !live && calls == 3 && exceptions == 1 &&
             checked_requests == 15;

  printf(
      "HOST_JNI_PROXY_RESULT=%s requests=%d calls=%d exceptions=%d "
      "live_refs=%d pending=%d "
      "errors=%d tid=%d\n",
      good ? "PASS" : "FAIL", checked_requests, calls, exceptions, live,
      pending, errors, real_tid);
  return good;
}
