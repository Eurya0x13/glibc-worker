#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include "host_internal.h"
#include "jni_signature.h"
#define FH_HANDLES 16384

/* Reference/ID table backing every jobject the bridge hands to the Guest.
   Locals are frame-scoped per context; method/field IDs are interned until
   their class unloads. The stored value is the raw VM reference — the
   caller keeps owning it (deleting it is the table's job via the drop
   paths, never the receiver's). */
struct handle {
  void *value;
  unsigned generation;
  int kind, owner, level;
  unsigned local_previous, local_next;
  jweak clazz;
  char sig[FB_SIG];
};
static struct handle handles[FH_HANDLES];
/* Index + 1, with zero as the end marker. Protected by handles_lock. */
static unsigned local_heads[FB_CONTEXTS];
static pthread_mutex_t handles_lock = PTHREAD_MUTEX_INITIALIZER;

/* Token = (generation << 16) | index + 1; zero is reserved as null. The
   generation advances on slot reuse so a stale token can never alias the
   handle that took its place. */
static jobject token(unsigned i) {
  return (jobject)(uintptr_t)(((uint64_t)handles[i].generation << 16) |
                              (i + 1));
}

/* Validate a token: NULL when out of range or the generation no longer
   matches (retired and reused slot). */
static struct handle *get_handle(jobject t) {
  uintptr_t v = (uintptr_t)t;
  unsigned i = (v & 65535);
  if (!i || i > FH_HANDLES) {
    return NULL;
  }
  struct handle *h = &handles[i - 1];
  return h->kind && h->generation == (v >> 16) ? h : NULL;
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

/* Take a VM reference into the table and return its token. Locals are
   pushed onto the owning context's list tagged with the current frame
   level, so frame pops retire exactly that level. Exhaustion returns NULL
   and counts an error. */
static jobject fh_add(struct host_context *c, void *value, int kind,
                      const char *sig) {
  if (!value) {
    return NULL;
  }
  pthread_mutex_lock(&handles_lock);

  for (unsigned i = 0; i < FH_HANDLES; i++) {
    if (!handles[i].kind) {
      struct handle *h = &handles[i];
      h->generation++;
      h->kind = kind;
      h->value = value;
      h->owner = c ? c->id : -1;
      h->level = c ? c->local_depth : 0;
      if (kind == H_LOCAL) {
        unsigned next = local_heads[c->id];
        h->local_previous = 0;
        h->local_next = next;
        if (next) {
          handles[next - 1].local_previous = i + 1;
        }
        local_heads[c->id] = i + 1;
      }
      h->sig[0] = 0;
      if (sig) {
        snprintf(h->sig, FB_SIG, "%s", sig);
      }

      jobject t = token(i);
      adjust_count(kind, 1);
      pthread_mutex_unlock(&handles_lock);
      return t;
    }
  }

  pthread_mutex_unlock(&handles_lock);
  atomic_fetch_add(&gw_host_bridge->errors, 1);
  return NULL;
}

jobject fh_add_ref(struct host_context *c, jobject value, int kind) {
  return fh_add(c, value, kind, NULL);
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

  for (unsigned i = 0; i < FH_HANDLES; i++) {
    if (handles[i].kind == kind && handles[i].value == value &&
        (*env)->IsSameObject(env, handles[i].clazz, clazz)) {
      jobject t = token(i);
      pthread_mutex_unlock(&handles_lock);
      return t;
    }
  }

  for (unsigned i = 0; i < FH_HANDLES; i++) {
    if (!handles[i].kind) {
      jweak weak = (*env)->NewWeakGlobalRef(env, clazz);
      if (!weak) {
        pthread_mutex_unlock(&handles_lock);
        return NULL;
      }
      struct handle *h = &handles[i];
      h->generation++;
      h->kind = kind;
      h->value = value;
      h->clazz = weak;
      h->owner = -1;
      h->level = 0;
      snprintf(h->sig, sizeof h->sig, "%s", sig ? sig : "");
      atomic_fetch_add(&gw_host_bridge->id_cache_weak, 1);
      jobject t = token(i);
      pthread_mutex_unlock(&handles_lock);
      return t;
    }
  }

  pthread_mutex_unlock(&handles_lock);
  atomic_fetch_add(&gw_host_bridge->errors, 1);
  return NULL;
}

/* Drop ID-cache entries whose declaring class was collected — a cleared
   jweak compares SameObject to NULL — or, with all=1, every entry
   (shutdown). Runs on a thread with a live JNIEnv. */
void sweep_ids(JNIEnv *env, int all) {
  pthread_mutex_lock(&handles_lock);

  for (unsigned i = 0; i < FH_HANDLES; i++) {
    struct handle *h = &handles[i];
    if (h->kind >= H_METHOD &&
        (all || (*env)->IsSameObject(env, h->clazz, NULL))) {
      (*env)->DeleteWeakGlobalRef(env, h->clazz);
      h->clazz = NULL;
      h->kind = 0;
      atomic_fetch_sub(&gw_host_bridge->id_cache_weak, 1);
    }
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

/* Unlink before making the slot reusable: DeleteLocalRef may be followed by
   another allocation in the same frame, including at the same handle index. */
static void retire_handle(struct handle *h) {
  if (h->kind == H_LOCAL) {
    if (h->local_previous) {
      handles[h->local_previous - 1].local_next = h->local_next;
    } else {
      local_heads[h->owner] = h->local_next;
    }
    if (h->local_next) {
      handles[h->local_next - 1].local_previous = h->local_previous;
    }
  }

  adjust_count(h->kind, -1);
  h->kind = 0;
}

void fh_drop_token(jobject t) {
  if (!t) {
    return;
  }

  pthread_mutex_lock(&handles_lock);
  struct handle *h = get_handle(t);
  if (h) {
    retire_handle(h);
  } else {
    atomic_fetch_add(&gw_host_bridge->errors, 1);
  }
  pthread_mutex_unlock(&handles_lock);
}

/* Retire this context's locals created at "level" or deeper — called when a
   local frame is popped. */
void fh_drop_locals(struct host_context *c, int level) {
  pthread_mutex_lock(&handles_lock);
  for (unsigned entry = local_heads[c->id]; entry;) {
    struct handle *h = &handles[entry - 1];
    entry = h->local_next;
    if (h->level >= level) {
      retire_handle(h);
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
    snprintf(sig, sizeof sig, "%s", p);
    (*env)->ReleaseStringUTFChars(env, text, p);
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
