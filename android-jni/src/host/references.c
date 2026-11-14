#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include "host_internal.h"
#include "jni_signature.h"
#include "slot_store.h"

/* C owns VM references and signatures; the backend owns stable slots and
   dense per-context local indexes. All accesses hold handles_lock. Tokens
   are private opaque values, never raw VM pointers or Guest dereferences. */
struct handle {
  void *value;
  int kind, owner, level;
  jweak clazz;
  char sig[FB_SIG];
};
static struct gw_slot_store *handles;
static pthread_mutex_t handles_lock = PTHREAD_MUTEX_INITIALIZER;

static struct handle *get_handle(jobject token) {
  return gw_slot_get(handles, (uint64_t)(uintptr_t)token);
}

static struct handle *insert_handle(int owner, uint64_t *token) {
  if (!handles) {
    handles = fh_slot_create(sizeof(struct handle), _Alignof(struct handle),
                             FB_CONTEXTS, 16384);
  }
  return gw_slot_insert(handles, owner, token);
}

static void handle_error(void) {
  if (gw_host_bridge) {
    atomic_fetch_add(&gw_host_bridge->errors, 1);
  }
}

static void adjust_count(int kind, int delta) {
  if (!gw_host_bridge) {
    return;
  }

  if (kind == H_LOCAL) {
    atomic_fetch_add(&gw_host_bridge->live_local, delta);
  } else if (kind == H_GLOBAL) {
    atomic_fetch_add(&gw_host_bridge->live_global, delta);
  } else if (kind == H_WEAK) {
    atomic_fetch_add(&gw_host_bridge->live_weak, delta);
  }
}

/* On allocation failure the caller retains ownership of the VM reference.
   Local values may be borrowed (native parameters), so this layer must not
   delete them. Generated NewGlobalRef/NewWeakGlobalRef callers roll back. */
jobject fh_add_ref(struct host_context *c, jobject value, int kind) {
  if (!value) {
    return NULL;
  }
  if (kind < H_LOCAL || kind > H_WEAK ||
      (kind == H_LOCAL && (!c || c->id < 0 || c->id >= FB_CONTEXTS))) {
    handle_error();
    return NULL;
  }
  pthread_mutex_lock(&handles_lock);
  uint64_t token;
  struct handle *h = insert_handle(kind == H_LOCAL ? c->id : -1, &token);
  if (h) {
    h->value = value;
    h->kind = kind;
    h->owner = c ? c->id : -1;
    h->level = c ? c->local_depth : 0;
    adjust_count(kind, 1);
  }
  pthread_mutex_unlock(&handles_lock);
  if (!h) {
    handle_error();
  }
  return (jobject)(uintptr_t)token;
}

/* Intern a method/field ID keyed by (kind, value, same declaring class).
   Interned entries carry owner -1 and no frame level: they are not tied to
   any local frame and stay valid until the class is collected (sweep_ids)
   or the bridge shuts down. */
jobject fh_add_id(struct host_context *c, void *value, int kind,
                  const char *sig, jclass clazz) {
  if (!value || !clazz) {
    return NULL;
  }
  JNIEnv *env = c->env;
  pthread_mutex_lock(&handles_lock);

  if ((kind != H_METHOD && kind != H_FIELD) ||
      (sig && strnlen(sig, FB_SIG) >= FB_SIG)) {
    pthread_mutex_unlock(&handles_lock);
    handle_error();
    return NULL;
  }
  for (size_t i = 0; i < gw_slot_capacity(handles); i++) {
    uint64_t token;
    struct handle *h = gw_slot_at(handles, i, &token);
    if (h && h->kind == kind && h->value == value &&
        (*env)->IsSameObject(env, h->clazz, clazz)) {
      pthread_mutex_unlock(&handles_lock);
      return (jobject)(uintptr_t)token;
    }
  }

  jweak weak = (*env)->NewWeakGlobalRef(env, clazz);
  if (!weak) {
    pthread_mutex_unlock(&handles_lock);
    return NULL;
  }
  uint64_t token;
  struct handle *h = insert_handle(-1, &token);
  if (h) {
    h->value = value;
    h->kind = kind;
    h->owner = -1;
    h->clazz = weak;
    memcpy(h->sig, sig ? sig : "", sig ? strlen(sig) + 1 : 1);
    atomic_fetch_add(&gw_host_bridge->id_cache_weak, 1);
  } else {
    (*env)->DeleteWeakGlobalRef(env, weak);
    handle_error();
  }
  pthread_mutex_unlock(&handles_lock);
  return (jobject)(uintptr_t)token;
}

/* Drop ID-cache entries whose declaring class was collected — a cleared
   jweak compares SameObject to NULL — or, with all=1, every entry
   (shutdown). Runs on a thread with a live JNIEnv. */
void sweep_ids(JNIEnv *env, int all) {
  pthread_mutex_lock(&handles_lock);

  for (size_t i = 0; i < gw_slot_capacity(handles); i++) {
    uint64_t token;
    struct handle *h = gw_slot_at(handles, i, &token);
    if (h && h->kind >= H_METHOD &&
        (all || (*env)->IsSameObject(env, h->clazz, NULL))) {
      (*env)->DeleteWeakGlobalRef(env, h->clazz);
      gw_slot_erase(handles, token);
      atomic_fetch_sub(&gw_host_bridge->id_cache_weak, 1);
    }
  }
  if (all && gw_slot_destroy(handles)) {
    handles = NULL;
  }

  pthread_mutex_unlock(&handles_lock);
}

/* Reflection bridge to BridgeTypes.declaringClass; NULL means a pending
   exception, which the caller must observe before using JNI again. */
jclass fh_declaring_class(struct host_context *c, jobject member) {
  JNIEnv *env = c->env;
  jclass cls = (jclass)(*env)->CallStaticObjectMethod(
      env, gw_host_helper_class, gw_host_declaring_method, member);
  if ((*env)->ExceptionCheck(env)) {
    return NULL;
  }
  return cls;
}

/* Token -> raw VM reference. Locals resolve only for their owning context:
   a JNI local reference is invalid off its own thread, so cross-context use
   is reported as an error rather than returned. */
jobject fh_ref(struct host_context *c, jobject t) {
  if (!t) {
    return NULL;
  }

  pthread_mutex_lock(&handles_lock);
  struct handle *h = get_handle(t);
  void *v = NULL;
  if (h && h->kind <= H_WEAK && (h->kind != H_LOCAL || h->owner == c->id)) {
    v = h->value;
  } else {
    atomic_fetch_add(&gw_host_bridge->errors, 1);
  }
  pthread_mutex_unlock(&handles_lock);
  return v;
}

/* Token -> interned method/field pointer, requiring the exact kind; a
   mismatched or stale token yields NULL plus an error count. */
void *fh_id(jobject t, int kind) {
  pthread_mutex_lock(&handles_lock);
  struct handle *h = get_handle(t);
  void *v = h && h->kind == kind ? h->value : NULL;
  pthread_mutex_unlock(&handles_lock);
  if (!v) {
    atomic_fetch_add(&gw_host_bridge->errors, 1);
  }
  return v;
}

/* Erasing swaps the last owner index into the removed position; no linked
   list traversal or allocation is needed to retire a local. */
static void retire_handle(struct handle *h, uint64_t token) {
  adjust_count(h->kind, -1);
  gw_slot_erase(handles, token);
}

void fh_drop_token(jobject token) {
  if (!token) {
    return;
  }
  pthread_mutex_lock(&handles_lock);
  struct handle *h = get_handle(token);
  if (h) {
    retire_handle(h, (uint64_t)(uintptr_t)token);
  } else {
    handle_error();
  }
  pthread_mutex_unlock(&handles_lock);
}

/* Only this context's contiguous token vector is visited, regardless of the
   total handle capacity. Revisit a position after swap-removing its item. */
void fh_drop_locals(struct host_context *c, int level) {
  pthread_mutex_lock(&handles_lock);
  size_t position = 0;
  uint64_t token;
  struct handle *h;
  while ((h = gw_slot_owned(handles, c->id, position, &token))) {
    if (h->level >= level) {
      retire_handle(h, token);
    } else {
      position++;
    }
  }
  pthread_mutex_unlock(&handles_lock);
}

/* Reflection bridge to BridgeTypes.describeMember. Returns a thread-local
   buffer: valid only until the next fh_describe on this thread; "" on
   failure. */
const char *fh_describe(struct host_context *c, jobject member) {
  static __thread char sig[FB_SIG];
  JNIEnv *env = c->env;
  sig[0] = 0;

  jstring text = (jstring)(*env)->CallStaticObjectMethod(
      env, gw_host_helper_class, gw_host_describe_method, member);
  if ((*env)->ExceptionCheck(env) || !text) {
    return sig;
  }

  const char *p = (*env)->GetStringUTFChars(env, text, NULL);
  if (p) {
    size_t length = strnlen(p, sizeof sig);
    if (length < sizeof sig) {
      memcpy(sig, p, length + 1);
    }
    (*env)->ReleaseStringUTFChars(env, text, p);
    if (length == sizeof sig) {
      (*env)->DeleteLocalRef(env, text);
      jclass error =
          (*env)->FindClass(env, "java/lang/IllegalArgumentException");
      if (error) {
        (*env)->ThrowNew(env, error, "JNI descriptor exceeds bridge capacity");
        (*env)->DeleteLocalRef(env, error);
      }
      return sig;
    }
  }
  (*env)->DeleteLocalRef(env, text);
  return sig;
}

/* Decode call arguments for a method token. Variant 'A' reads a prepared
   jvalue array; otherwise a va_list is widened per the stored descriptor
   (JNI promotes sub-int and float arguments). Object arguments arrive as
   tokens and are resolved with fh_ref. Returns 0 when the handle is not a
   method, its signature no longer parses, or required args are absent. */
int fh_decode(struct host_context *c, jobject id, jobject raw, char variant,
              jvalue *out) {
  char sig[FB_SIG], types[FB_ARGS], ret;
  int n;

  pthread_mutex_lock(&handles_lock);
  struct handle *h = get_handle(id);
  snprintf(sig, sizeof sig, "%s", h && h->kind == H_METHOD ? h->sig : "");
  pthread_mutex_unlock(&handles_lock);
  if (!sig_types(sig, types, &n, &ret)) {
    return 0;
  }

  if (variant == 'A') {
    if (n && !raw) {
      return 0;
    }
    if (n) {
      memcpy(out, raw, n * sizeof *out);
    }
  } else {
    va_list va;
    va_copy(va, *(va_list *)raw);

    for (int i = 0; i < n; i++) {
      out[i] = (jvalue){0};
      switch (types[i]) {
        case 'Z':
          out[i].z = (jboolean)va_arg(va, int);
          break;
        case 'B':
          out[i].b = (jbyte)va_arg(va, int);
          break;
        case 'C':
          out[i].c = (jchar)va_arg(va, int);
          break;
        case 'S':
          out[i].s = (jshort)va_arg(va, int);
          break;
        case 'I':
          out[i].i = va_arg(va, jint);
          break;
        case 'J':
          out[i].j = va_arg(va, jlong);
          break;
        case 'F':
          out[i].f = (jfloat)va_arg(va, double);
          break;
        case 'D':
          out[i].d = va_arg(va, double);
          break;
        default:
          out[i].l = va_arg(va, jobject);
          break;
      }
    }
    va_end(va);
  }

  for (int i = 0; i < n; i++) {
    if (types[i] == 'L') {
      out[i].l = fh_ref(c, out[i].l);
    }
  }
  return 1;
}
