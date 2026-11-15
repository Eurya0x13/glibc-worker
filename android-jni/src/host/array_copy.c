#include "host_internal.h"
#include <stdio.h>
#include <stdlib.h>

/* Copies are legal JNI Elements results. Only Host allocation and JNI calls
   live here; no JVM heap pointer, JNIEnv or reference is cached. */
#define COPY_SLOTS 8
/* Android allocator page reclamation also affects 1 MiB JNI copies. Keep
   eligible buffers live between leases; the slot and byte limits still bound
   retention. The threshold is a policy, not a JVM allocation boundary.

   Scudo MapAllocatorCache::store can release pages but retain the mapping;
   oversized entries are unmapped. A reused address need not have resident
   pages. Keeping the allocation live avoids repeated free/refault cycles.
   Android 13 source (release interval is set by the mallopt wrapper):
   https://android.googlesource.com/platform/external/scudo/+/8e2d762272cd7bed1362cbe22016d1d9754d74f8/standalone/secondary.h
   https://android.googlesource.com/platform/external/scudo/+/8e2d762272cd7bed1362cbe22016d1d9754d74f8/standalone/wrappers_c.inc

   HotSpot's Elements path allocates/copies and frees on final release:
   https://github.com/openjdk/jdk17u/blob/jdk-17.0.14-ga/src/hotspot/share/prims/jni.cpp#L2235
   This cache uses Region calls for the required copy; it never pins the heap.
 */
#define COPY_MIN (1u * 1024u * 1024u)
#define COPY_MAX (8u * 1024u * 1024u)
#define COPY_BUDGET (32u * 1024u * 1024u)

static struct copy_slot {
  void *pointer;
  size_t capacity;
  jsize length;
  int kind, active;
} slots[COPY_SLOTS];

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int enabled;
static size_t allocated;
static unsigned long hits, misses, fallback;

void fh_array_copy_start(int use_cache) {
  enabled = use_cache;
  printf(
      "ARRAY_COPY_CACHE enabled=%d min_bytes=%u max_bytes=%u budget_bytes=%u "
      "slots=%d\n",
      enabled, COPY_MIN, COPY_MAX, COPY_BUDGET, COPY_SLOTS);
}

static struct copy_slot *checkout(jsize length, size_t width, int kind) {
  size_t bytes = (size_t)length * width;
  if (bytes < COPY_MIN || bytes > COPY_MAX) {
    return NULL;
  }

  pthread_mutex_lock(&lock);
  struct copy_slot *chosen = NULL;
  for (int i = 0; i < COPY_SLOTS; i++) {
    struct copy_slot *s = &slots[i];
    if (!s->active && s->pointer && s->capacity >= bytes &&
        (!chosen || s->capacity < chosen->capacity)) {
      chosen = s;
    }
  }

  if (chosen) {
    hits++;
    chosen->active = 1;
    chosen->length = length;
    chosen->kind = kind;
    pthread_mutex_unlock(&lock);
    return chosen;
  }

  for (int i = 0; i < COPY_SLOTS; i++) {
    struct copy_slot *s = &slots[i];
    if (!s->active && allocated - s->capacity + bytes <= COPY_BUDGET) {
      chosen = s;
      break;
    }
  }

  if (!chosen) {
    fallback++;
    pthread_mutex_unlock(&lock);
    return NULL;
  }

  void *old = chosen->pointer;
  allocated = allocated - chosen->capacity + bytes;
  *chosen = (struct copy_slot){
      .capacity = bytes, .length = length, .kind = kind, .active = 1};
  misses++;
  pthread_mutex_unlock(&lock);
  free(old);
  void *pointer = malloc(bytes);
  pthread_mutex_lock(&lock);
  chosen->pointer = pointer;
  if (!pointer) {
    allocated -= bytes;
    *chosen = (struct copy_slot){0};
    fallback++;
  }
  pthread_mutex_unlock(&lock);
  return pointer ? chosen : NULL;
}

static void recycle(struct copy_slot *slot) {
  pthread_mutex_lock(&lock);
  slot->active = 0;
  pthread_mutex_unlock(&lock);
}

static struct copy_slot *lookup(void *pointer, int kind) {
  struct copy_slot *found = NULL;
  pthread_mutex_lock(&lock);

  for (int i = 0; i < COPY_SLOTS; i++) {
    if (slots[i].active && slots[i].pointer == pointer &&
        slots[i].kind == kind) {
      found = &slots[i];
      break;
    }
  }

  pthread_mutex_unlock(&lock);
  return found;
}

/* No lock spans a VM call: the VM can safepoint or reenter the bridge. An
   acquired buffer remains checked out through JNI_COMMIT and across threads. */
// clang-format off
#define ARRAY_COPY(Type, type, kind)                                      \
  j##type *fh_get_##type##_elements(JNIEnv *env, j##type##Array array,    \
                                    jboolean *copy) {                     \
    if (!enabled) {                                                       \
      return (*env)->Get##Type##ArrayElements(env, array, copy);          \
    }                                                                     \
                                                                          \
    jsize length = (*env)->GetArrayLength(env, array);                    \
    if ((*env)->ExceptionCheck(env)) {                                    \
      return NULL;                                                        \
    }                                                                     \
                                                                          \
    struct copy_slot *s = checkout(length, sizeof(j##type), kind);        \
    if (!s) {                                                             \
      return (*env)->Get##Type##ArrayElements(env, array, copy);          \
    }                                                                     \
                                                                          \
    (*env)->Get##Type##ArrayRegion(env, array, 0, length, s->pointer);    \
    if ((*env)->ExceptionCheck(env)) {                                    \
      recycle(s);                                                         \
      return NULL;                                                        \
    }                                                                     \
    if (copy) {                                                           \
      *copy = JNI_TRUE;                                                   \
    }                                                                     \
    return s->pointer;                                                    \
  }                                                                       \
                                                                          \
  void fh_release_##type##_elements(JNIEnv *env, j##type##Array array,    \
                                    j##type *pointer, jint mode) {        \
    struct copy_slot *s = enabled ? lookup(pointer, kind) : NULL;         \
    if (!s) {                                                             \
      (*env)->Release##Type##ArrayElements(env, array, pointer, mode);    \
      return;                                                             \
    }                                                                     \
                                                                          \
    if (mode != JNI_ABORT) {                                              \
      /* Release is permitted with a pending exception; Region is not. */ \
      jthrowable pending = (*env)->ExceptionOccurred(env);                \
      if (pending) {                                                      \
        (*env)->ExceptionClear(env);                                      \
      }                                                                   \
                                                                          \
      (*env)->Set##Type##ArrayRegion(env, array, 0, s->length, pointer);  \
      if (pending) {                                                      \
        if ((*env)->ExceptionCheck(env)) {                                \
          (*env)->ExceptionClear(env);                                    \
        }                                                                 \
        (*env)->Throw(env, pending);                                      \
        (*env)->DeleteLocalRef(env, pending);                             \
      }                                                                   \
    }                                                                     \
    if (mode != JNI_COMMIT) {                                             \
      recycle(s);                                                         \
    }                                                                     \
  }
// clang-format on

ARRAY_COPY(Boolean, boolean, 1)
ARRAY_COPY(Byte, byte, 2)
ARRAY_COPY(Char, char, 3)
ARRAY_COPY(Short, short, 4)
ARRAY_COPY(Int, int, 5)
ARRAY_COPY(Long, long, 6)
ARRAY_COPY(Float, float, 7)
ARRAY_COPY(Double, double, 8)
#undef ARRAY_COPY

void fh_array_copy_finish(void) {
  unsigned active = 0;
  pthread_mutex_lock(&lock);
  for (int i = 0; i < COPY_SLOTS; i++) {
    if (slots[i].active) {
      active++;
      continue; /* An invalid outstanding lease must never become a UAF. */
    }
    free(slots[i].pointer);
    allocated -= slots[i].capacity;
    slots[i] = (struct copy_slot){0};
  }
  printf(
      "ARRAY_COPY_CACHE_FINISH hits=%lu misses=%lu fallback=%lu active=%u "
      "retained_bytes=%zu\n",
      hits, misses, fallback, active, allocated);
  pthread_mutex_unlock(&lock);
}
