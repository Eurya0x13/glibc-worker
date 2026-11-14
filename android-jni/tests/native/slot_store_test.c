/* Production reference logic with a VM test double; storage fault injection
   uses the real private ABI. Android execution is covered separately. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host_internal.h"
#include "slot_store.h"

static struct full_bridge counters;
struct full_bridge *gw_host_bridge = &counters;
jclass gw_host_helper_class;
jmethodID gw_host_describe_method, gw_host_declaring_method;
static size_t attempts, fail_at = SIZE_MAX, outstanding, failures, weak_count;
static int collected, pending_exception, string_releases;
static const char *reflection_signature;

static void *allocate(void *context, size_t size, size_t alignment) {
  (void)context;
  if (attempts++ == fail_at) {
    failures++;
    return NULL;
  }
  void *p;
  if (alignment < sizeof(void *)) {
    alignment = sizeof(void *);
  }
  if (posix_memalign(&p, alignment, size)) {
    return NULL;
  }
  outstanding++;
  return p;
}

static void deallocate(void *context, void *memory) {
  (void)context;
  assert(memory && outstanding);
  outstanding--;
  free(memory);
}
static struct gw_slot_config config = {.object_size = 32,
                                       .object_alignment = 16,
                                       .owner_count = 3,
                                       .control_limit = 192,
                                       .generation_max = UINT32_MAX,
                                       .allocate = allocate,
                                       .deallocate = deallocate};

static void storage_failures(void) {
  for (size_t fault = 0; fault < 48; fault++) {
    attempts = 0;
    fail_at = fault;
    struct gw_slot_store *s = gw_slot_create(&config);
    uint64_t tokens[160] = {0};
    if (s) {
      for (size_t i = 0; i < 160; i++) {
        uint64_t *p = gw_slot_insert(s, (int)(i % 3), &tokens[i]);
        if (p) {
          *p = i + 1;
        } else {
          assert(!tokens[i]);
          fail_at = SIZE_MAX;
          p = gw_slot_insert(s, (int)(i % 3), &tokens[i]);
          assert(p);
          *p = i + 1;
        }
      }
      assert(!gw_slot_destroy(s));
      for (size_t i = 0; i < 160; i++) {
        assert(*(uint64_t *)gw_slot_get(s, tokens[i]) == i + 1);
        assert(gw_slot_erase(s, tokens[i]));
        assert(!gw_slot_get(s, tokens[i]) && !gw_slot_erase(s, tokens[i]));
      }
      for (int owner = 0; owner < 3; owner++) {
        uint64_t token = 123;
        assert(!gw_slot_owned(s, owner, 0, &token) && !token);
      }
      assert(gw_slot_destroy(s));
    }
    fail_at = SIZE_MAX;
    assert(!outstanding);
  }
  assert(failures >= 8);
}

static void generations_and_owners(void) {
  struct gw_slot_config c = config;
  c.generation_max = 2;
  c.control_limit = 64;
  struct gw_slot_store *s = gw_slot_create(&c);
  assert(s);
  uint64_t stale[128];
  for (size_t i = 0; i < 128; i++) {
    assert(gw_slot_insert(s, 1, &stale[i]));
    assert(gw_slot_erase(s, stale[i]));
    for (size_t j = 0; j <= i; j++) {
      assert(!gw_slot_get(s, stale[j]));
    }
  }
  uint64_t token;
  void *p = gw_slot_insert(s, 1, &token);
  if (GW_TEST_ZIG) {
    assert(p && (uint32_t)token > 64);
    assert(gw_slot_erase(s, token));
  } else {
    assert(!p && !token);
  }
  assert(gw_slot_destroy(s) && !outstanding);

  s = gw_slot_create(&config);
  uint64_t tokens[160];
  void *first = NULL;
  for (size_t i = 0; i < 160; i++) {
    void *object = gw_slot_insert(s, (int)(i % 3), &tokens[i]);
    assert(object);
    if (!i) {
      first = object;
    }
    assert(gw_slot_get(s, tokens[0]) == first);
  }
  for (size_t i = 1; i < 160; i += 2) {
    assert(gw_slot_erase(s, tokens[i]));
  }
  size_t seen = 0;
  for (int owner = 0; owner < 3; owner++) {
    while (gw_slot_owned(s, owner, 0, &token)) {
      assert(gw_slot_erase(s, token));
      seen++;
    }
  }
  assert(seen == 80 && gw_slot_destroy(s) && !outstanding);
  token = 123;
  assert(!gw_slot_insert(NULL, -1, &token) && !token);
  assert(!gw_slot_create(NULL));
  c = config;
  c.object_size = SIZE_MAX - 15;
  s = gw_slot_create(&c);
  if (s) {
    assert(!gw_slot_insert(s, -1, &token));
    assert(gw_slot_destroy(s));
  }
  assert(!outstanding);
}

struct vm_class {
  int identity;
};

static jweak new_weak(JNIEnv *env, jobject object) {
  (void)env;
  struct vm_class *weak = malloc(sizeof *weak);
  assert(weak);
  *weak = *(struct vm_class *)object;
  weak_count++;
  return (jweak)weak;
}

static void delete_weak(JNIEnv *env, jweak weak) {
  (void)env;
  assert(weak_count);
  weak_count--;
  free(weak);
}

static jboolean same_object(JNIEnv *env, jobject a, jobject b) {
  (void)env;
  int ai = a ? ((struct vm_class *)a)->identity : 0;
  int bi = b ? ((struct vm_class *)b)->identity : 0;
  if (collected && ai == 1) {
    ai = 0;
  }
  if (collected && bi == 1) {
    bi = 0;
  }
  return ai == bi;
}

static jobject describe_member(JNIEnv *env, jclass cls, jmethodID method, ...) {
  (void)env;
  (void)cls;
  (void)method;
  return (jobject)reflection_signature;
}

static const char *get_chars(JNIEnv *env, jstring value, jboolean *copy) {
  (void)env;
  (void)copy;
  return (const char *)value;
}

static void release_chars(JNIEnv *env, jstring value, const char *text) {
  (void)env;
  (void)value;
  (void)text;
  string_releases++;
}

static jboolean has_exception(JNIEnv *env) {
  (void)env;
  return pending_exception;
}

static jclass error_class(JNIEnv *env, const char *name) {
  (void)env;
  assert(!strcmp(name, "java/lang/IllegalArgumentException"));
  return (jclass)(uintptr_t)1;
}

static jint throw_error(JNIEnv *env, jclass cls, const char *message) {
  (void)env;
  (void)cls;
  (void)message;
  pending_exception = 1;
  return JNI_OK;
}

static void delete_local(JNIEnv *env, jobject object) {
  (void)env;
  (void)object;
}
static const struct JNINativeInterface_ vm = {
    .NewWeakGlobalRef = new_weak,
    .DeleteWeakGlobalRef = delete_weak,
    .IsSameObject = same_object,
    .CallStaticObjectMethod = describe_member,
    .GetStringUTFChars = get_chars,
    .ReleaseStringUTFChars = release_chars,
    .ExceptionCheck = has_exception,
    .FindClass = error_class,
    .ThrowNew = throw_error,
    .DeleteLocalRef = delete_local};
static JNIEnv env = &vm;

static void references(void) {
  struct host_context contexts[2] = {{.env = &env, .id = 0},
                                     {.env = &env, .id = 1}};
  size_t count = GW_TEST_ZIG ? 32768 : 16384;
  jobject *tokens = calloc(count, sizeof *tokens);
  assert(tokens);
  for (size_t i = 0; i < count; i++) {
    struct host_context *c = &contexts[i % 2];
    c->local_depth = (int)(i % 3) + 1;
    tokens[i] = fh_add_ref(c, (jobject)(uintptr_t)(i + 1), H_LOCAL);
    assert(tokens[i]);
    assert(fh_ref(c, tokens[i]) == (jobject)(uintptr_t)(i + 1));
  }
  assert(atomic_load(&counters.live_local) == (int)count);
  if (!GW_TEST_ZIG) {
    assert(!fh_add_ref(&contexts[0], (jobject)(uintptr_t)1, H_LOCAL));
  }
  assert(!fh_ref(&contexts[1], tokens[0]));
  assert(!fh_id(tokens[0], H_METHOD));
  fh_drop_token(tokens[123]);
  assert(!fh_ref(&contexts[1], tokens[123]));
  fh_drop_locals(&contexts[0], 2);
  for (size_t i = 0; i < count; i += 2) {
    if (i % 3 == 0) {
      assert(fh_ref(&contexts[0], tokens[i]) == (jobject)(uintptr_t)(i + 1));
    }
  }
  fh_drop_locals(&contexts[0], 0);
  assert(atomic_load(&counters.live_local) == (int)(count / 2 - 1));
  fh_drop_locals(&contexts[1], 0);
  assert(!atomic_load(&counters.live_local));
  jobject global = fh_add_ref(&contexts[0], (jobject)(uintptr_t)71, H_GLOBAL);
  jobject weak = fh_add_ref(&contexts[0], (jobject)(uintptr_t)72, H_WEAK);
  assert(global && weak &&
         fh_ref(&contexts[1], global) == (jobject)(uintptr_t)71);
  assert(atomic_load(&counters.live_global) == 1 &&
         atomic_load(&counters.live_weak) == 1);
  fh_drop_token(global);
  fh_drop_token(weak);
  assert(!atomic_load(&counters.live_global) &&
         !atomic_load(&counters.live_weak));
  struct vm_class a = {1}, alias = {1}, b = {2};
  jobject method = fh_add_id(&contexts[0], (void *)(uintptr_t)17, H_METHOD,
                             "(I)I", (jclass)&a);
  assert(method && fh_add_id(&contexts[1], (void *)(uintptr_t)17, H_METHOD,
                             "(I)I", (jclass)&alias) == method);
  jobject other = fh_add_id(&contexts[0], (void *)(uintptr_t)17, H_METHOD,
                            "(I)I", (jclass)&b);
  jobject field =
      fh_add_id(&contexts[0], (void *)(uintptr_t)17, H_FIELD, "I", (jclass)&a);
  assert(other && other != method && field && field != method &&
         weak_count == 3);
  assert(fh_id(method, H_METHOD) == (void *)(uintptr_t)17);
  char too_long[FB_SIG + 1];
  memset(too_long, 'I', FB_SIG);
  too_long[FB_SIG] = 0;
  assert(!fh_add_id(&contexts[0], (void *)(uintptr_t)18, H_METHOD, too_long,
                    (jclass)&a));
  assert(weak_count == 3);
  collected = 1;
  sweep_ids(&env, 0);
  assert(weak_count == 1 && !fh_id(method, H_METHOD));
  assert(fh_id(other, H_METHOD) == (void *)(uintptr_t)17);
  sweep_ids(&env, 1);
  assert(!weak_count && !atomic_load(&counters.id_cache_weak));
  free(tokens);
}

static void reflection_boundaries(void) {
  struct host_context c = {.env = &env};
  char text[FB_SIG + 1];
  memset(text, 'I', sizeof text);
  text[FB_SIG - 1] = 0;
  reflection_signature = text;
  const char *value = fh_describe(&c, (jobject)(uintptr_t)1);
  assert(strlen(value) == FB_SIG - 1 && !pending_exception &&
         string_releases == 1);
  text[FB_SIG - 1] = 'I';
  text[FB_SIG] = 0;
  value = fh_describe(&c, (jobject)(uintptr_t)1);
  assert(!value[0] && pending_exception && string_releases == 2);
  pending_exception = 0;
}

int main(void) {
  storage_failures();
  generations_and_owners();
  references();
  reflection_boundaries();
  printf(
      "{\"result\":\"PASS\",\"backend\":\"%s\",\"references\":%u,"
      "\"oom_cases\":48,\"injected_failures\":%zu,\"generation_exhaustion\":"
      "true,"
      "\"owner_swap_remove\":true,\"outstanding_allocations\":%zu,\"weak_"
      "refs\":%zu}\n",
      GW_TEST_ZIG ? "zig" : "c", GW_TEST_ZIG ? 32768u : 16384u, failures,
      outstanding, weak_count);
  return 0;
}
