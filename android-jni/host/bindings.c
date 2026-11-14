#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include "host_internal.h"
#include "jni_signature.h"

/* Upper bound on live native-method trampolines per process; exhausting the
   table turns into a GW_E_CAPACITY load failure, not a silent miss. */
#define FH_NATIVE 512

static struct binding bindings[FH_NATIVE];
static pthread_mutex_t bindings_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t registrations_lock = PTHREAD_MUTEX_INITIALIZER;

struct registered_class {
  uint64_t module;
  jweak clazz;
};

static struct registered_class registrations[256];

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

/* Return an existing binding for the same (module, guest address,
   signature) or build one: the cif mirrors the JNI (env, receiver, args)
   prologue and the closure binds this file's ffi_native with the binding as
   context. Fails only on table exhaustion, a rejected signature, or a
   libffi error; the slot is released on every failure path. */
static struct binding *make_binding_unlocked(uint64_t module, uintptr_t guest,
                                             const char *sig) {
  for (int i = 0; i < FH_NATIVE; i++) {
    if (bindings[i].used && bindings[i].module == module &&
        bindings[i].guest == guest && !strcmp(bindings[i].sig, sig)) {
      return &bindings[i];
    }
  }

  struct binding *b = NULL;
  for (int i = 0; i < FH_NATIVE; i++) {
    if (!bindings[i].used) {
      b = &bindings[i];
      b->used = 1;
      break;
    }
  }
  if (!b) {
    return NULL;
  }

  if (!sig_types(sig, b->types, &b->nargs, &b->ret)) {
    b->used = 0;
    return NULL;
  }

  snprintf(b->sig, sizeof b->sig, "%s", sig);

  b->guest = guest;
  b->module = module;
  b->args[0] = b->args[1] = &ffi_type_pointer;
  for (int j = 0; j < b->nargs; j++) {
    b->args[j + 2] = jni_ffi_type(b->types[j]);
  }

  if (ffi_prep_cif(&b->cif, FFI_DEFAULT_ABI, b->nargs + 2, jni_ffi_type(b->ret),
                   b->args) != FFI_OK) {
    b->used = 0;
    return NULL;
  }

  b->closure = ffi_closure_alloc(sizeof(ffi_closure), &b->code);
  if (!b->closure || ffi_prep_closure_loc(b->closure, &b->cif, ffi_native, b,
                                          b->code) != FFI_OK) {
    if (b->closure) {
      ffi_closure_free(b->closure);
    }
    b->used = 0;
    return NULL;
  }

  atomic_fetch_add(&gw_host_bridge->live_closures, 1);
  return b;
}

/* Slot creation and teardown are serialized here; trampolines only ever read
   fields frozen at creation, so ffi_native needs no lock. */
struct binding *make_binding(uint64_t module, uintptr_t guest,
                             const char *sig) {
  pthread_mutex_lock(&bindings_lock);
  struct binding *b = make_binding_unlocked(module, guest, sig);
  pthread_mutex_unlock(&bindings_lock);
  return b;
}

/* A binding still marked active is executing on another thread: leak its
   closure and count an error instead of freeing memory the VM may be
   calling into. */
void release_bindings(uint64_t module) {
  pthread_mutex_lock(&bindings_lock);

  for (int i = 0; i < FH_NATIVE; i++) {
    if (bindings[i].used && bindings[i].module == module) {
      if (atomic_load(&bindings[i].active)) {
        atomic_fetch_add(&gw_host_bridge->errors, 1);
      } else {
        ffi_closure_free(bindings[i].closure);
        memset(&bindings[i], 0, sizeof bindings[i]);
        atomic_fetch_sub(&gw_host_bridge->live_closures, 1);
      }
    }
  }

  pthread_mutex_unlock(&bindings_lock);
}

/* One registering library owns a declaring class until its registrations
   retire. Weak refs track ownership without keeping a class loader alive. */
static int own_registration(JNIEnv *env, jclass cls, uint64_t module) {
  int free_index = -1;
  pthread_mutex_lock(&registrations_lock);

  for (int i = 0; i < 256; i++) {
    if (!registrations[i].module) {
      if (free_index < 0) {
        free_index = i;
      }
      continue;
    }
    if ((*env)->IsSameObject(env, registrations[i].clazz, cls)) {
      int ok = registrations[i].module == module;
      pthread_mutex_unlock(&registrations_lock);
      return ok;
    }
  }

  if (free_index >= 0) {
    jweak weak = (*env)->NewWeakGlobalRef(env, cls);
    if (weak) {
      registrations[free_index] =
          (struct registered_class){.module = module, .clazz = weak};
      pthread_mutex_unlock(&registrations_lock);
      return 1;
    }
  }

  pthread_mutex_unlock(&registrations_lock);
  return 0;
}

/* UnregisterNatives needs a strong jclass, so the weak ref is revived first;
   a cleared weak skips the unregister call but is still deleted. */
void release_registrations(JNIEnv *env, uint64_t module) {
  pthread_mutex_lock(&registrations_lock);

  for (int i = 0; i < 256; i++) {
    if (registrations[i].module == module) {
      jobject cls = (*env)->NewLocalRef(env, registrations[i].clazz);
      if (cls) {
        if ((*env)->UnregisterNatives(env, (jclass)cls) != JNI_OK) {
          atomic_fetch_add(&gw_host_bridge->errors, 1);
        }
        (*env)->DeleteLocalRef(env, cls);
      }
      (*env)->DeleteWeakGlobalRef(env, registrations[i].clazz);
      memset(&registrations[i], 0, sizeof registrations[i]);
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
  if (!owner || !own_registration(c->env, cls, owner)) {
    return JNI_ERR;
  }

  JNINativeMethod copy[64];
  for (int i = 0; i < n; i++) {
    struct binding *b =
        make_binding(owner, (uintptr_t)methods[i].fnPtr, methods[i].signature);
    if (!b) {
      return JNI_ERR;
    }
    copy[i] = methods[i];
    copy[i].fnPtr = b->code;
  }

  return (*c->env)->RegisterNatives(c->env, cls, copy, n);
}
