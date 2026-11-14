#include "jni_proxy.h"
#include <stdarg.h>
#include <stddef.h>

static __thread struct shared *owner;
static const struct JNINativeInterface env_table;
static const struct JNIInvokeInterface vm_table;
static __thread JNIEnv proxy_env = &env_table;
static JavaVM proxy_vm = &vm_table;

static int copy_text(char *dst, size_t cap, const char *src) {
  if (!src) {
    return 0;
  }

  for (size_t i = 0; i < cap; i++) {
    dst[i] = src[i];
    if (!src[i]) {
      return 1;
    }
  }
  return 0;
}

static int64_t rpc(int op, uintptr_t object, uintptr_t method, int arg,
                   const char *name, const char *signature) {
  if (!owner) {
    return 0;
  }
  struct jni_rpc *r = &owner->jni;
  if ((name && !copy_text(r->name, sizeof r->name, name)) ||
      (signature && !copy_text(r->signature, sizeof r->signature, signature))) {
    owner->jni_bridge_error = 1;
    return 0;
  }

  r->op = op;
  r->object = object;
  r->method = method;
  r->arg = arg;
  r->error = 0;
  r->result = 0;

  int sequence = atomic_load_explicit(&r->request, memory_order_relaxed) + 1;
  atomic_store_explicit(&r->request, sequence, memory_order_release);
  while (atomic_load_explicit(&r->response, memory_order_acquire) != sequence) {
    __asm__ volatile("" ::: "memory");
  }

  if (r->error) {
    owner->jni_bridge_error = 1;
  }
  return r->result;
}

static jint get_env(JavaVM *vm, void **env, jint version) {
  if (!env) {
    return JNI_ERR;
  }
  *env = NULL;
  if (vm != &proxy_vm) {
    return JNI_ERR;
  }
  if (version != JNI_VERSION_1_6) {
    return JNI_EVERSION;
  }
  if (!owner) {
    return JNI_EDETACHED;
  }

  *env = &proxy_env;
  return JNI_OK;
}

static jint destroy_vm(JavaVM *vm) {
  (void)vm;
  return JNI_ERR;
}

static jint attach_vm(JavaVM *vm, JNIEnv **env, void *args) {
  (void)vm;
  (void)args;

  if (env) {
    *env = NULL;
  }

  return JNI_ERR; /* Real ART attach and new virtual contexts unsupported. */
}

static jint detach_vm(JavaVM *vm) {
  (void)vm;
  return JNI_ERR;
}

static jint get_version(JNIEnv *env) {
  (void)env;
  return (jint)rpc(J_VERSION, 0, 0, 0, NULL, NULL);
}

static jclass find_class(JNIEnv *env, const char *name) {
  (void)env;
  return (jclass)(uintptr_t)rpc(J_FIND_CLASS, 0, 0, 0, name, NULL);
}

static jmethodID get_static(JNIEnv *env, jclass cls, const char *name,
                            const char *sig) {
  (void)env;
  return (jmethodID)(uintptr_t)rpc(J_STATIC_METHOD, (uintptr_t)cls, 0, 0, name,
                                   sig);
}

static jint call_int_a(JNIEnv *env, jclass cls, jmethodID method,
                       const jvalue *args) {
  (void)env;
  /* Only the (I)I signature accepted by Host, with exactly one jvalue. */
  return (jint)rpc(J_CALL_INT, (uintptr_t)cls, (uintptr_t)method,
                   args ? args[0].i : 0, NULL, NULL);
}

static jint call_int_v(JNIEnv *env, jclass cls, jmethodID method,
                       va_list args) {
  jvalue v = {.i = va_arg(args, jint)};
  return call_int_a(env, cls, method, &v);
}

static jint call_int(JNIEnv *env, jclass cls, jmethodID method, ...) {
  va_list args;
  va_start(args, method);
  jint result = call_int_v(env, cls, method, args);
  va_end(args);

  return result;
}

static jboolean exception_check(JNIEnv *env) {
  (void)env;
  return (jboolean)rpc(J_EXCEPTION_CHECK, 0, 0, 0, NULL, NULL);
}

static void exception_clear(JNIEnv *env) {
  (void)env;
  rpc(J_EXCEPTION_CLEAR, 0, 0, 0, NULL, NULL);
}

static void delete_local(JNIEnv *env, jobject ref) {
  (void)env;
  rpc(J_DELETE_LOCAL, (uintptr_t)ref, 0, 0, NULL, NULL);
}

static jint get_vm(JNIEnv *env, JavaVM **vm) {
  if (!owner || env != &proxy_env || !vm) {
    return JNI_ERR;
  }

  *vm = &proxy_vm;

  return JNI_OK;
}
/* All unlisted JNI slots are NULL: deliberately a smoke harness, not an API
   implementation safe for arbitrary modules. Never use it for an unknown DSO.
 */
static const struct JNINativeInterface env_table = {
    .GetVersion = get_version,
    .FindClass = find_class,
    .GetStaticMethodID = get_static,
    .CallStaticIntMethod = call_int,
    .CallStaticIntMethodV = call_int_v,
    .CallStaticIntMethodA = call_int_a,
    .ExceptionCheck = exception_check,
    .ExceptionClear = exception_clear,
    .DeleteLocalRef = delete_local,
    .GetJavaVM = get_vm};
static const struct JNIInvokeInterface vm_table = {
    .DestroyJavaVM = destroy_vm,
    .AttachCurrentThread = attach_vm,
    .DetachCurrentThread = detach_vm,
    .GetEnv = get_env,
    .AttachCurrentThreadAsDaemon = attach_vm};

JavaVM *guest_jni_context_begin(struct shared *shared) {
  owner = shared;
  return &proxy_vm;
}

void guest_jni_context_end(void) {
  owner = NULL;
}
