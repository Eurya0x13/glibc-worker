#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include "host_internal.h"
#include "jni_signature.h"
#include "binding_table.h"
#include "slot_store.h"

static pthread_mutex_t bindings_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t registrations_lock = PTHREAD_MUTEX_INITIALIZER;

struct registered_class {
  uint64_t module;
  jweak clazz;
  size_t pending;
  int published;
};

static struct gw_slot_store *registrations;

/* The trampoline the VM actually calls for a registered native method: it
   translates one JNI call into an FB_NATIVE bridge command. jobject
   arguments are re-issued as Host handle tokens (H_LOCAL) so the Guest never
   holds a raw VM reference. "active" plus gw_loader_enter keep the module
   and its closure from being retired while the call is in flight. */
static void ffi_native(ffi_cif *cif, void *result, void **values,
                       void *opaque) {
  (void)cif;
  struct binding *b = opaque;
  JNIEnv *env = *(JNIEnv **)values[0];
  struct host_context *c = fh_context(env);

  if (!c) {
    /* A thread outside bridge management cannot have its call routed or
       attributed; return a zeroed result instead of throwing. */
    if (result) {
      memset(result, 0, 8);
    }
    return;
  }

  GW_RPC_SCOPE(boundary, GW_RPC_STAGE_COUNT, -1);
  GW_RPC_SCOPE(pre, GW_RPC_HOST_TRAMPOLINE_PRE, -1);

  atomic_fetch_add(&b->active, 1);
  if (!gw_loader_enter(b->module)) {
    jclass ex = (*env)->FindClass(env, "java/lang/UnsatisfiedLinkError");
    if (ex) {
      (*env)->ThrowNew(env, ex, "Guest library is retired");
    }
    atomic_fetch_sub(&b->active, 1);
    GW_RPC_SCOPE_END(pre);
    GW_RPC_SCOPE_END(boundary);
    return;
  }

  uint64_t previous = c->module;
  c->module = b->module;
  int prior = c->local_depth;
  if ((*env)->PushLocalFrame(env, 512) != JNI_OK) {
    c->module = previous;
    uint64_t module = b->module;
    atomic_fetch_sub(&b->active, 1);
    gw_loader_leave(module);
    GW_RPC_SCOPE_END(pre);
    GW_RPC_SCOPE_END(boundary);
    return;
  }

  c->local_depth++;
  /* PushLocalFrame bounds every local created while inside the guest call;
     PopLocalFrame below keeps only the returned object alive. Tokens minted
     here are tagged with local_depth so fh_drop_locals retires exactly this
     frame. */
  jvalue a[FB_ARGS + 2] = {{0}};
  a[0].l = fh_add_ref(c, *(jobject *)values[1], H_LOCAL);
  for (int i = 0; i < b->nargs; i++) {
    switch (b->types[i]) {
      case 'Z':
        a[i + 1].z = *(jboolean *)values[i + 2];
        break;
      case 'B':
        a[i + 1].b = *(jbyte *)values[i + 2];
        break;
      case 'C':
        a[i + 1].c = *(jchar *)values[i + 2];
        break;
      case 'S':
        a[i + 1].s = *(jshort *)values[i + 2];
        break;
      case 'I':
        a[i + 1].i = *(jint *)values[i + 2];
        break;
      case 'J':
        a[i + 1].j = *(jlong *)values[i + 2];
        break;
      case 'F':
        a[i + 1].f = *(jfloat *)values[i + 2];
        break;
      case 'D':
        a[i + 1].d = *(jdouble *)values[i + 2];
        break;
      default:
        a[i + 1].l = fh_add_ref(c, *(jobject *)values[i + 2], H_LOCAL);
        break;
    }
  }

  GW_RPC_SCOPE_END(pre);
  jvalue out = fh_command(c, FB_NATIVE, b->guest, b->sig, a);
  GW_RPC_SCOPE(post, GW_RPC_HOST_TRAMPOLINE_POST, -1);
  jobject ref = b->ret == 'L' ? fh_ref(c, out.l) : NULL;
  ref = (*env)->PopLocalFrame(env, ref);
  fh_drop_locals(c, prior + 1);
  c->local_depth = prior;

  if (b->ret == 'L') {
    out.l = ref;
  }
  if (b->ret != 'V') {
    /* libffi returns sub-word integers in a word-sized slot; only the
       floating types have narrower representations. */
    memcpy(result, &out,
           b->ret == 'F'   ? sizeof(jfloat)
           : b->ret == 'D' ? sizeof(jdouble)
                           : sizeof(ffi_arg));
  }

  c->module = previous;
  uint64_t module = b->module;
  atomic_fetch_sub(&b->active, 1);
  gw_loader_leave(module);
  GW_RPC_SCOPE_END(post);
  GW_RPC_SCOPE_END(boundary);
}

/* Shared by the C control and Zig pilot. The registry supplies stable,
   zero-initialized storage and publishes it only after preparation succeeds. */
int fh_binding_prepare(struct binding *b, uint64_t module, uintptr_t guest,
                       const char *sig) {
  size_t length = strnlen(sig, sizeof b->sig);
  if (length == sizeof b->sig) {
    return GW_BINDING_ARGUMENT;
  }
  if (!sig_types(sig, b->types, &b->nargs, &b->ret)) {
    return GW_BINDING_ARGUMENT;
  }

  memcpy(b->sig, sig, length + 1);

  b->guest = guest;
  b->module = module;
  atomic_init(&b->active, 0);
  b->args[0] = b->args[1] = &ffi_type_pointer;
  for (int j = 0; j < b->nargs; j++) {
    b->args[j + 2] = jni_ffi_type(b->types[j]);
  }

  if (ffi_prep_cif(&b->cif, FFI_DEFAULT_ABI, b->nargs + 2, jni_ffi_type(b->ret),
                   b->args) != FFI_OK) {
    return GW_BINDING_FFI;
  }

  b->closure = ffi_closure_alloc(sizeof(ffi_closure), &b->code);
  if (!b->closure || ffi_prep_closure_loc(b->closure, &b->cif, ffi_native, b,
                                          b->code) != FFI_OK) {
    if (b->closure) {
      ffi_closure_free(b->closure);
    }
    b->closure = NULL;
    return GW_BINDING_FFI;
  }

  atomic_fetch_add(&gw_host_bridge->live_closures, 1);
  return GW_BINDING_OK;
}

/* Quiescence is a caller precondition; active detects a violated contract,
   but cannot protect a VM thread about to enter a stale executable address. */
int fh_binding_retire(struct binding *b) {
  if (atomic_load(&b->active)) {
    atomic_fetch_add(&gw_host_bridge->errors, 1);
    return 0;
  }
  ffi_closure_free(b->closure);
  atomic_fetch_sub(&gw_host_bridge->live_closures, 1);
  return 1;
}

static void binding_error(struct gw_loader_error *error, int status) {
  int code = status == GW_BINDING_MEMORY     ? GW_E_CAPACITY
             : status == GW_BINDING_ARGUMENT ? GW_E_ARGUMENT
                                             : GW_E_BRIDGE;
  const char *text =
      status == GW_BINDING_MEMORY     ? "Cannot allocate native binding storage"
      : status == GW_BINDING_ARGUMENT ? "Invalid or oversized native descriptor"
                                      : "Cannot prepare native libffi closure";
  gw_loader_fail(error, code, text);
}

struct binding *make_binding(uint64_t module, uintptr_t guest, const char *sig,
                             struct gw_loader_error *error) {
  if (!module || !guest || !sig || strnlen(sig, FB_SIG) >= FB_SIG) {
    binding_error(error, GW_BINDING_ARGUMENT);
    return NULL;
  }
  pthread_mutex_lock(&bindings_lock);
  int created, status;
  struct binding *b = binding_get(module, guest, sig, &created, &status);
  pthread_mutex_unlock(&bindings_lock);
  if (!b) {
    binding_error(error, status);
  }
  return b;
}

void release_bindings(uint64_t module) {
  pthread_mutex_lock(&bindings_lock);
  binding_release(module);
  pthread_mutex_unlock(&bindings_lock);
}

void fh_bindings_finish(void) {
  pthread_mutex_lock(&bindings_lock);
  binding_finish();
  pthread_mutex_unlock(&bindings_lock);
  pthread_mutex_lock(&registrations_lock);
  if (gw_slot_destroy(registrations)) {
    registrations = NULL;
  }
  pthread_mutex_unlock(&registrations_lock);
}

/* One registering library owns a declaring class until its registrations
   retire. Weak refs track ownership without keeping a class loader alive. */
static uint64_t own_registration(JNIEnv *env, jclass cls, uint64_t module) {
  pthread_mutex_lock(&registrations_lock);
  for (size_t i = 0; i < gw_slot_capacity(registrations); i++) {
    uint64_t token;
    struct registered_class *record = gw_slot_at(registrations, i, &token);
    if (record && (*env)->IsSameObject(env, record->clazz, cls)) {
      if (record->module != module || record->pending == SIZE_MAX) {
        token = 0;
      } else {
        record->pending++;
      }
      pthread_mutex_unlock(&registrations_lock);
      return token;
    }
  }
  if (!registrations) {
    registrations = fh_slot_create(sizeof(struct registered_class),
                                   _Alignof(struct registered_class), 0, 256);
  }
  uint64_t token = 0;
  struct registered_class *record = gw_slot_insert(registrations, -1, &token);
  if (record) {
    jweak weak = (*env)->NewWeakGlobalRef(env, cls);
    if (weak) {
      *record = (struct registered_class){
          .module = module, .clazz = weak, .pending = 1};
    } else {
      gw_slot_erase(registrations, token);
      token = 0;
    }
  }
  pthread_mutex_unlock(&registrations_lock);
  return token;
}

/* A failed preparation must not reserve a class forever. Parallel attempts
   share a pending count; once any attempt reaches the VM the record stays,
   because RegisterNatives may publish a prefix even when it returns JNI_ERR. */
static void finish_registration(JNIEnv *env, uint64_t token, int published) {
  pthread_mutex_lock(&registrations_lock);
  struct registered_class *record = gw_slot_get(registrations, token);
  if (record) {
    record->published |= published;
    record->pending--;
    if (!record->pending && !record->published) {
      (*env)->DeleteWeakGlobalRef(env, record->clazz);
      gw_slot_erase(registrations, token);
    }
  }
  pthread_mutex_unlock(&registrations_lock);
}

/* UnregisterNatives needs a strong jclass, so the weak ref is revived first;
   a cleared weak skips the unregister call but is still deleted. */
void release_registrations(JNIEnv *env, uint64_t module) {
  pthread_mutex_lock(&registrations_lock);

  for (size_t i = 0; i < gw_slot_capacity(registrations); i++) {
    uint64_t token;
    struct registered_class *record = gw_slot_at(registrations, i, &token);
    if (record && record->module == module) {
      jobject cls = (*env)->NewLocalRef(env, record->clazz);
      if (cls) {
        if ((*env)->UnregisterNatives(env, (jclass)cls) != JNI_OK) {
          atomic_fetch_add(&gw_host_bridge->errors, 1);
        }
        (*env)->DeleteLocalRef(env, cls);
      }
      (*env)->DeleteWeakGlobalRef(env, record->clazz);
      gw_slot_erase(registrations, token);
    }
  }

  pthread_mutex_unlock(&registrations_lock);
}

/* The owning module is the calling guest when known; otherwise the Guest is
   asked which image owns fnPtr (FB_OWNER). fnPtr entries are swapped for
   trampoline addresses in a copy, so the caller's array is left unchanged;
   per the class-ownership rule above, a class may only register once. */
jint fh_register(struct host_context *c, jclass cls,
                 const JNINativeMethod *methods, jint n) {
  if (n < 0 || n > 64) {
    return JNI_ERR;
  }

  uint64_t owner = c->module;
  if (!owner && n) {
    owner = (uint64_t)fh_command(c, FB_OWNER, (uintptr_t)methods[0].fnPtr, NULL,
                                 NULL)
                .j;
  }
  uint64_t registration = owner ? own_registration(c->env, cls, owner) : 0;
  if (!registration) {
    return JNI_ERR;
  }

  JNINativeMethod copy[64];
  struct binding *fresh[64];
  int fresh_count = 0;
  pthread_mutex_lock(&bindings_lock);
  for (int i = 0; i < n; i++) {
    int created = 0, status = GW_BINDING_ARGUMENT;
    const char *sig = methods[i].signature;
    struct binding *b = NULL;
    if (sig && methods[i].fnPtr && strnlen(sig, FB_SIG) < FB_SIG) {
      b = binding_get(owner, (uintptr_t)methods[i].fnPtr, sig, &created,
                      &status);
    }
    if (!b) {
      while (fresh_count) {
        binding_discard(fresh[--fresh_count]);
      }
      pthread_mutex_unlock(&bindings_lock);
      finish_registration(c->env, registration, 0);
      if (!(*c->env)->ExceptionCheck(c->env)) {
        const char *name = status == GW_BINDING_MEMORY
                               ? "java/lang/OutOfMemoryError"
                           : status == GW_BINDING_ARGUMENT
                               ? "java/lang/IllegalArgumentException"
                               : "java/lang/UnsatisfiedLinkError";
        jclass ex = (*c->env)->FindClass(c->env, name);
        if (ex) {
          (*c->env)->ThrowNew(c->env, ex, "Cannot prepare native bindings");
          (*c->env)->DeleteLocalRef(c->env, ex);
        }
      }
      return JNI_ERR;
    }
    if (created) {
      fresh[fresh_count++] = b;
    }
    copy[i] = methods[i];
    copy[i].fnPtr = b->code;
  }
  pthread_mutex_unlock(&bindings_lock);

  /* A VM may install a prefix before reporting failure. Once passed to the
     VM these closures stay owned by the module, even on JNI_ERR. */
  jint result = (*c->env)->RegisterNatives(c->env, cls, copy, n);
  finish_registration(c->env, registration, 1);
  return result;
}
