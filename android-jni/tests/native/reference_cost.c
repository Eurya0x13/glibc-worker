/* CPU cost of production reference paths with a minimal VM double.
 * No RPC or actual VM work: timings isolate storage, scans and locking. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "host_internal.h"
#include "slot_store.h"

static struct full_bridge bridge;
struct full_bridge *gw_host_bridge = &bridge;
jclass gw_host_helper_class;
jmethodID gw_host_describe_method, gw_host_declaring_method;
static struct gw_slot_store *store;
static size_t allocations, weak_refs;

static void *allocate(void *context, size_t bytes, size_t alignment) {
  (void)context;
  void *memory;

  if (alignment < sizeof(void *)) {
    alignment = sizeof(void *);
  }
  if (posix_memalign(&memory, alignment, bytes)) {
    return NULL;
  }
  allocations++;
  return memory;
}

static void deallocate(void *context, void *memory) {
  (void)context;
  free(memory);
}

struct gw_slot_store *fh_slot_create(size_t bytes, size_t alignment,
                                     size_t owners, size_t initial) {
  struct gw_slot_config config = {.object_size = bytes,
                                  .object_alignment = alignment,
                                  .owner_count = owners,
                                  .initial_capacity = initial,
                                  .generation_max = UINT32_MAX,
                                  .allocate = allocate,
                                  .deallocate = deallocate};
  store = gw_slot_create(&config);
  return store;
}

void fh_throw(JNIEnv *env, enum fb_failure kind, const char *message) {
  (void)env;
  (void)kind;
  fprintf(stderr, "Unexpected benchmark failure: %s\n", message);
  abort();
}

static jweak new_weak(JNIEnv *env, jobject value) {
  (void)env;
  weak_refs++;
  return value;
}

static void delete_weak(JNIEnv *env, jweak value) {
  (void)env;
  (void)value;
  assert(weak_refs);
  weak_refs--;
}

static jboolean same_object(JNIEnv *env, jobject a, jobject b) {
  (void)env;
  return a == b;
}

#ifdef __ANDROID__
static const struct JNINativeInterface vm = {
#else
static const struct JNINativeInterface_ vm = {
#endif
    .NewWeakGlobalRef = new_weak,
    .DeleteWeakGlobalRef = delete_weak,
    .IsSameObject = same_object};

static uint64_t now(void) {
  struct timespec value;
  assert(!clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value));
  return (uint64_t)value.tv_sec * 1000000000 + value.tv_nsec;
}

static void measure(struct host_context *c, const char *name, int op,
                    jobject expected, size_t high_water, unsigned trial) {
  unsigned count = op == 2 ? 256 : 1000;
  size_t before_allocations = allocations;
  uint64_t start = now();

  for (unsigned i = 0; i < count; i++) {
    if (op == 0) {
      assert(fh_ref(c, expected) == (jobject)(uintptr_t)1);
    } else if (op == 1 || op == 3) {
      uintptr_t value = op == 1 ? 17 : 18;
      assert(fh_add_id(c, (void *)value, H_METHOD, "()V",
                       (jclass)(uintptr_t)1) == expected);
    } else {
      assert(fh_add_id(c, (void *)(uintptr_t)(1000 + trial * count + i),
                       H_METHOD, "()V", (jclass)(uintptr_t)1));
    }
  }

  uint64_t elapsed = now() - start;
  printf(
      "{\"operation\":\"%s\",\"high_water_refs\":%zu,"
      "\"capacity\":%zu,\"trial\":%u,\"calls\":%u,\"cpu_ns\":%llu,"
      "\"allocations\":%zu}\n",
      name, high_water, gw_slot_capacity(store), trial, count,
      (unsigned long long)elapsed, allocations - before_allocations);
}

int main(int argc, char **argv) {
  assert(argc == 2);
  char *end;
  size_t high_water = strtoul(argv[1], &end, 10);
  assert(*argv[1] && !*end && high_water <= 262144);

  JNIEnv env = &vm;
  struct host_context c = {.env = &env};
  jobject early = fh_add_id(&c, (void *)(uintptr_t)17, H_METHOD, "()V",
                            (jclass)(uintptr_t)1);
  assert(early);

  jobject *tokens = calloc(high_water + 1, sizeof *tokens);
  assert(tokens);
  for (size_t i = 0; i < high_water; i++) {
    tokens[i] = fh_add_ref(&c, (jobject)(uintptr_t)1, H_GLOBAL);
    assert(tokens[i]);
  }
  jobject late = fh_add_id(&c, (void *)(uintptr_t)18, H_METHOD, "()V",
                           (jclass)(uintptr_t)1);
  assert(late);

  for (size_t i = 0; i < high_water; i++) {
    fh_drop_token(tokens[i]);
  }
  free(tokens);

  jobject local = fh_add_ref(&c, (jobject)(uintptr_t)1, H_LOCAL);
  assert(local);
  for (unsigned trial = 0; trial < 11; trial++) {
    measure(&c, "reference_get", 0, local, high_water, trial);
    measure(&c, "id_hit_early", 1, early, high_water, trial);
    measure(&c, "id_hit_late", 3, late, high_water, trial);
    measure(&c, "id_miss", 2, NULL, high_water, trial);
  }

  fh_drop_locals(&c, 0);
  sweep_ids(&env, 1);
  assert(!weak_refs && !atomic_load(&bridge.live_global) &&
         !atomic_load(&bridge.live_local) && !atomic_load(&bridge.errors));
  return 0;
}
