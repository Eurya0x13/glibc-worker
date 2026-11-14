/* Real Host libffi closures and production JNI marshalling, with a small VM/
   RPC test double. This is not an Android or dual-libc execution test. */
#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "binding_table.h"
#include "resource_limits.h"

static struct full_bridge counters;
struct full_bridge *gw_host_bridge = &counters;
static _Thread_local struct host_context context;
static int exception_pending, register_calls, reject_registration;
static int closure_failure = -1, weak_failure, local_failure,
           unregister_failure;
static size_t registration_weaks;
static int quarantined, context_calls, context_failure, frame_failure,
    reference_failure, rpc_calls, memory_failure;
static const char *exception_class;
static int mutation_probe, mutation_checks, unregister_calls;
static uint64_t reverse_owner[4];
static const JNINativeMethod *mutation_methods;

int gw_dlopen_is_quarantined(void) {
  return quarantined;
}

void gw_dlopen_quarantine(void) {
  quarantined = 1;
}

static size_t allocation_attempts, fail_at = SIZE_MAX, outstanding;
static size_t injected_failures;

int __real_posix_memalign(void **, size_t, size_t);

int __wrap_posix_memalign(void **p, size_t alignment, size_t size) {
  return memory_failure ? 12 : __real_posix_memalign(p, alignment, size);
}

void *__real_ffi_closure_alloc(size_t, void **);

void *__wrap_ffi_closure_alloc(size_t size, void **code) {
  if (closure_failure == 0) {
    return NULL;
  }
  if (closure_failure > 0) {
    closure_failure--;
  }
  return __real_ffi_closure_alloc(size, code);
}

static jint push_frame(JNIEnv *env, jint capacity) {
  (void)capacity;
  if (frame_failure) {
    fh_throw(env, FB_FAILURE_MEMORY, "test frame allocation failure");
    return JNI_ERR;
  }
  return JNI_OK;
}

static jobject pop_frame(JNIEnv *env, jobject value) {
  (void)env;
  return value;
}

static jboolean same_object(JNIEnv *env, jobject a, jobject b) {
  (void)env;
  return a == b;
}

static jclass find_class(JNIEnv *env, const char *name) {
  (void)env;
  exception_class = name;
  return (jclass)(uintptr_t)1;
}

static jint throw_new(JNIEnv *env, jclass cls, const char *text) {
  (void)env;
  (void)cls;
  (void)text;
  exception_pending = 1;
  return JNI_OK;
}

static jboolean exception_check(JNIEnv *env) {
  (void)env;
  return exception_pending;
}

static void delete_ref(JNIEnv *env, jobject object) {
  (void)env;
  (void)object;
}

static jweak weak_ref(JNIEnv *env, jobject object) {
  (void)env;
  if (weak_failure) {
    exception_pending = 1;
    return NULL;
  }
  registration_weaks++;
  return (jweak)object;
}

static void delete_weak(JNIEnv *env, jweak object) {
  (void)env;
  (void)object;
  assert(registration_weaks);
  registration_weaks--;
}

static jobject local_ref(JNIEnv *env, jobject object) {
  (void)env;
  if (local_failure) {
    exception_pending = 1;
    return NULL;
  }
  return object;
}

static jint register_methods(JNIEnv *env, jclass cls,
                             const JNINativeMethod *methods, jint count) {
  (void)env;
  (void)cls;
  assert(count == 3 && methods[0].fnPtr && methods[2].fnPtr);
  register_calls++;
  if (mutation_probe == 1) {
    mutation_probe = 0;
    assert(fh_unregister(&context, cls) == JNI_ERR);
    exception_pending = 0;
    assert(fh_register(&context, cls, methods, count) == JNI_ERR);
    exception_pending = 0;
    mutation_checks += 2;
  }
  return reject_registration ? JNI_ERR : JNI_OK;
}

static jint unregister_methods(JNIEnv *env, jclass cls) {
  (void)env;
  (void)cls;
  unregister_calls++;
  if (mutation_probe == 2) {
    mutation_probe = 0;
    assert(fh_register(&context, cls, mutation_methods, 3) == JNI_ERR);
    exception_pending = 0;
    assert(fh_unregister(&context, cls) == JNI_ERR);
    exception_pending = 0;
    mutation_checks += 2;
  }
  return unregister_failure ? JNI_ERR : JNI_OK;
}

static const struct JNINativeInterface_ vm = {
    .PushLocalFrame = push_frame,
    .PopLocalFrame = pop_frame,
    .IsSameObject = same_object,
    .FindClass = find_class,
    .ThrowNew = throw_new,
    .ExceptionCheck = exception_check,
    .DeleteLocalRef = delete_ref,
    .NewWeakGlobalRef = weak_ref,
    .DeleteWeakGlobalRef = delete_weak,
    .NewLocalRef = local_ref,
    .RegisterNatives = register_methods,
    .UnregisterNatives = unregister_methods};
static JNIEnv env = &vm;

struct host_context *fh_context(JNIEnv *environment,
                                struct gw_loader_error *error) {
  (void)error;
  context_calls++;
  if (context_failure) {
    fh_throw(environment, FB_FAILURE_MEMORY, "test context capacity failure");
    return NULL;
  }
  context.env = environment;
  return &context;
}

jobject fh_add_ref(struct host_context *c, jobject value, int kind) {
  (void)kind;
  if (reference_failure && value) {
    c->marshal_error = FB_FAILURE_MEMORY;
    fh_throw(c->env, FB_FAILURE_MEMORY, "test token allocation failure");
    return NULL;
  }
  return value;
}

jobject fh_ref(struct host_context *c, jobject value) {
  (void)c;
  return value;
}

void fh_drop_locals(struct host_context *c, int depth) {
  (void)c;
  (void)depth;
}

jvalue fh_command(struct host_context *c, int op, uintptr_t guest,
                  const char *signature, jvalue *arguments) {
  rpc_calls++;
  if (op == FB_OWNER) {
    return (jvalue){.j = guest < 4 ? (jlong)reverse_owner[guest] : 0};
  }
  assert(op == FB_NATIVE && !strcmp(signature, "(J)J"));
  return (jvalue){.j = arguments[1].j + (jlong)guest + (jlong)c->module};
}

int gw_loader_scope_enter(uint64_t module) {
  assert(module);
  return 1;
}

int gw_loader_scope_leave(uint64_t module) {
  assert(module);
  return 1;
}

int gw_loader_fail(struct gw_loader_error *error, int code, const char *text) {
  if (error) {
    error->code = code;
    snprintf(error->message, sizeof error->message, "%s", text);
  }
  return GW_ERROR;
}

static void invoke(struct binding *b) {
  JNIEnv *environment = &env;
  jobject receiver = (jobject)(uintptr_t)9;
  jlong argument = 123, result = 0;
  void *values[] = {&environment, &receiver, &argument};
  ffi_call(&b->cif, FFI_FN(b->code), &result, values);
  assert(result == argument + (jlong)b->guest + (jlong)b->module);
}

static void *allocate(void *unused, size_t size, size_t alignment) {
  (void)unused;
  if (allocation_attempts++ == fail_at) {
    injected_failures++;
    return NULL;
  }
  void *memory = NULL;
  if (alignment < sizeof(void *)) {
    alignment = sizeof(void *);
  }
  if (posix_memalign(&memory, alignment, size)) {
    return NULL;
  }
  outstanding++;
  return memory;
}

static void deallocate(void *unused, void *memory) {
  (void)unused;
  assert(outstanding && memory);
  outstanding--;
  free(memory);
}

static int prepare(void *unused, void *object, uint64_t module, uintptr_t guest,
                   const char *signature) {
  (void)unused;
  return fh_binding_prepare(object, module, guest, signature);
}

static int retire(void *unused, void *object) {
  (void)unused;
  return fh_binding_retire(object);
}

static const struct gw_binding_callbacks callbacks = {
    .object_size = sizeof(struct binding),
    .object_alignment = _Alignof(struct binding),
    .signature_offset = offsetof(struct binding, sig),
    .signature_capacity = FB_SIG,
    .initial_capacity = GW_BINDING_INITIAL_CAPACITY,
    .allocate = allocate,
    .deallocate = deallocate,
    .prepare = prepare,
    .retire = retire};

static struct binding *get(struct gw_binding_store *store, uint64_t module,
                           uintptr_t guest) {
  void *object = NULL;
  int created = 0;
  assert(gw_binding_store_get(store, module, guest, "(J)J", 4, &object,
                              &created) == GW_BINDING_OK);
  return object;
}

static void initial_pool(void) {
  struct gw_binding_store *store = gw_binding_store_create(&callbacks);
  assert(store && gw_binding_store_capacity(store) == 512);
  assert(!atomic_load(&counters.live_closures));
  struct binding *first = NULL;
  for (uint64_t module = 1; module <= 64; module++) {
    for (uintptr_t guest = 1; guest <= 8; guest++) {
      struct binding *b = get(store, module, guest);
      if (!first) {
        first = b;
      }
    }
    assert(gw_binding_store_capacity(store) == 512);
  }
  assert(atomic_load(&counters.live_closures) == 512);
  assert(get(store, 1, 1) == first);
  invoke(first);
  get(store, 64, 9);
  assert(gw_binding_store_capacity(store) == 576);
  invoke(first);
  assert(gw_binding_store_release(store, 2) == GW_BINDING_OK);
  for (uintptr_t guest = 1; guest <= 8; guest++) {
    get(store, 65, guest);
  }
  assert(gw_binding_store_capacity(store) == 576);
  for (uint64_t module = 1; module <= 65; module++) {
    assert(gw_binding_store_release(store, module) == GW_BINDING_OK);
  }
  assert(gw_binding_store_destroy(store) == GW_BINDING_OK && !outstanding);
  for (size_t failure = 0; failure < 24; failure++) {
    fail_at = allocation_attempts + failure;
    store = gw_binding_store_create(&callbacks);
    fail_at = SIZE_MAX;
    if (store) {
      assert(gw_binding_store_destroy(store) == GW_BINDING_OK);
    }
    assert(!outstanding && !atomic_load(&counters.live_closures));
  }
}

static void store_stress(void) {
  struct gw_binding_store *store = gw_binding_store_create(&callbacks);
  assert(store);
  struct binding *first = get(store, 1, 1);
  void *code = first->code;
  for (uintptr_t i = 2; i <= 4096; i++) {
    get(store, 1, i);
    get(store, 2, i);
    if (i % 64 == 0) {
      assert(get(store, 1, 1) == first && first->code == code);
      invoke(first);
    }
  }
  assert(atomic_load(&counters.live_closures) == 8191);
  assert(get(store, 2, 1) != first);
  assert(gw_binding_store_destroy(store) == GW_BINDING_BUSY);
  assert(gw_binding_store_release(store, 2) == GW_BINDING_OK);
  invoke(first);
  atomic_store(&first->active, 1);
  assert(gw_binding_store_release(store, 1) == GW_BINDING_BUSY);
  assert(atomic_load(&counters.live_closures) == 1);
  atomic_store(&first->active, 0);
  invoke(first);
  assert(gw_binding_store_release(store, 1) == GW_BINDING_OK);
  assert(gw_binding_store_destroy(store) == GW_BINDING_OK);
  assert(!outstanding && !atomic_load(&counters.live_closures));
}

static void store_failures(void) {
  const size_t prefixes[] = {0, 12, 64, 512};
  for (size_t p = 0; p < sizeof prefixes / sizeof *prefixes; p++) {
    for (size_t failure = 0; failure < 10; failure++) {
      struct gw_binding_store *store = gw_binding_store_create(&callbacks);
      assert(store);
      struct binding *first = NULL;
      for (size_t i = 1; i <= prefixes[p]; i++) {
        struct binding *b = get(store, 11, i);
        if (!first) {
          first = b;
        }
      }
      fail_at = allocation_attempts + failure;
      void *object = (void *)(uintptr_t)1;
      int created = -1;
      int status = gw_binding_store_get(store, 11, prefixes[p] + 1, "(J)J", 4,
                                        &object, &created);
      fail_at = SIZE_MAX;
      assert(status == GW_BINDING_OK || status == GW_BINDING_MEMORY);
      if (status != GW_BINDING_OK) {
        assert(!object && !created);
      }
      if (first) {
        assert(get(store, 11, 1) == first);
        invoke(first);
      }
      invoke(get(store, 11, prefixes[p] + 1));
      assert(gw_binding_store_release(store, 11) == GW_BINDING_OK);
      assert(gw_binding_store_destroy(store) == GW_BINDING_OK);
      assert(!outstanding && !atomic_load(&counters.live_closures));
    }
  }
  fail_at = allocation_attempts;
  assert(!gw_binding_store_create(&callbacks));
  fail_at = SIZE_MAX;
  assert(injected_failures >= 8);

  struct gw_binding_store *store = gw_binding_store_create(&callbacks);
  void *object = NULL;
  int created;
  closure_failure = 0;
  assert(gw_binding_store_get(store, 12, 1, "(J)J", 4, &object, &created) ==
         GW_BINDING_MEMORY);
  closure_failure = -1;
  assert(!object && !created && !atomic_load(&counters.live_closures));
  get(store, 12, 1);
  char longest[FB_SIG];
  longest[0] = '(';
  longest[1] = 'L';
  memset(longest + 2, 'x', FB_SIG - 6);
  memcpy(longest + FB_SIG - 4, ";)J", 4);
  void *other = NULL;
  assert(gw_binding_store_get(store, 12, 1, longest, FB_SIG - 1, &other,
                              &created) == GW_BINDING_OK);
  assert(created && other != get(store, 12, 1));
  assert(!strcmp(((struct binding *)other)->sig, longest));
  char too_long[FB_SIG + 1];
  memset(too_long, 'I', sizeof too_long - 1);
  too_long[sizeof too_long - 1] = 0;
  assert(gw_binding_store_get(store, 12, 2, too_long, FB_SIG, &object,
                              &created) == GW_BINDING_ARGUMENT);
  assert(gw_binding_store_discard(store, 12, 1, "(J)J", 4) == GW_BINDING_OK);
  assert(gw_binding_store_release(store, 12) == GW_BINDING_OK);
  assert(gw_binding_store_destroy(store) == GW_BINDING_OK && !outstanding);
}

static void *caller(void *object) {
  for (int i = 0; i < 10000; i++) {
    invoke(object);
  }
  return NULL;
}

static void registration_contract(const JNINativeMethod *methods) {
  jclass cls = (jclass)(uintptr_t)7;
  mutation_methods = methods;
  mutation_probe = 1;
  assert(fh_register(&context, cls, methods, 3) == JNI_OK &&
         mutation_checks == 2);
  context.module = 23;
  assert(fh_unregister(&context, cls) == JNI_ERR && exception_pending);
  exception_pending = 0;
  context.module = 22;
  mutation_probe = 2;
  assert(fh_unregister(&context, cls) == JNI_OK && mutation_checks == 4);
  assert(!registration_weaks && atomic_load(&counters.live_closures) == 3);
  context.module = 23;
  assert(fh_register(&context, cls, methods, 3) == JNI_OK);
  int before = unregister_calls;
  assert(release_registrations(&env, 22) == JNI_OK &&
         unregister_calls == before);
  assert(fh_unregister(&context, cls) == JNI_OK);
  release_bindings(23);
  /* Untracked unregister also reserves the class against concurrent publish. */
  mutation_probe = 2;
  assert(fh_unregister(&context, cls) == JNI_OK && mutation_checks == 6);
  context.module = 0;
  reverse_owner[1] = reverse_owner[3] = 24;
  reverse_owner[2] = 25;
  assert(fh_register(&context, cls, methods, 3) == JNI_ERR);
  exception_pending = 0;
  reverse_owner[2] = 0;
  assert(fh_register(&context, cls, methods, 3) == JNI_ERR);
  exception_pending = 0;
  reverse_owner[2] = 24;
  assert(fh_register(&context, cls, methods, 3) == JNI_OK);
  assert(fh_unregister(&context, cls) == JNI_OK);
  release_bindings(24);
  context.module = 22;
  assert(fh_register(&context, cls, methods, 3) == JNI_OK);
}

static void uncertain_unregister(const JNINativeMethod *methods) {
  pid_t child = fork();
  assert(child >= 0);
  if (!child) {
    context.module = 31;
    jclass cls = (jclass)(uintptr_t)99999;
    assert(fh_register(&context, cls, methods, 3) == JNI_OK);
    size_t weak = registration_weaks;
    int live = atomic_load(&counters.live_closures);
    unregister_failure = 1;
    assert(fh_unregister(&context, cls) == JNI_ERR && quarantined);
    assert(registration_weaks == weak &&
           atomic_load(&counters.live_closures) == live);
    _exit(
        0); /* Quarantine deliberately retains resources until process exit. */
  }
  int status;
  assert(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
         !WEXITSTATUS(status));
}

static void production_backend(void) {
  struct gw_loader_error error = {0};
  struct binding *first = make_binding(21, 1, "(J)J", &error);
  assert(first);
  pthread_t thread;
  assert(!pthread_create(&thread, NULL, caller, first));
  const size_t limit = 4096;
  for (size_t i = 2; i <= limit; i++) {
    assert(make_binding(21, i, "(J)J", &error));
  }
  assert(!pthread_join(thread, NULL));
  assert(make_binding(21, 1, "(J)J", &error) == first);
  invoke(first);
  int prior_contexts = context_calls;
  quarantined = 1;
  typedef jlong (*native_fn)(JNIEnv *, jobject, jlong);
  assert(((native_fn)first->code)(&env, NULL, 5) == 0);
  assert(exception_pending && context_calls == prior_contexts &&
         !atomic_load(&first->active));
  quarantined = exception_pending = 0;
  frame_failure = 1;
  int before_rpc = rpc_calls;
  assert(((native_fn)first->code)(&env, NULL, 5) == 0 && exception_pending);
  assert(!strcmp(exception_class, "java/lang/OutOfMemoryError") &&
         rpc_calls == before_rpc);
  frame_failure = exception_pending = 0;
  context.depth = FB_DEPTH + 20;
  invoke(first);
  before_rpc = rpc_calls;
  context.depth = 0;
  struct binding *object = make_binding(21, 2, "(Ljava/lang/Object;)J", &error);
  reference_failure = 1;
  typedef jlong (*object_fn)(JNIEnv *, jobject, jobject);
  assert(((object_fn)object->code)(&env, (jobject)(uintptr_t)1,
                                   (jobject)(uintptr_t)2) == 0 &&
         exception_pending);
  assert(!strcmp(exception_class, "java/lang/OutOfMemoryError") &&
         rpc_calls == before_rpc && !context.marshal_error);
  reference_failure = exception_pending = 0;
  struct binding *floating = make_binding(21, 3, "()F", &error);
  context_failure = 1;
  typedef jfloat (*float_fn)(JNIEnv *, jobject);
  assert(((float_fn)floating->code)(&env, NULL) == 0 && exception_pending);
  assert(!strcmp(exception_class, "java/lang/OutOfMemoryError") &&
         rpc_calls == before_rpc);
  context_failure = exception_pending = 0;
  release_bindings(21);
  assert(!atomic_load(&counters.live_closures));

  context = (struct host_context){.env = &env, .module = 22};
  JNINativeMethod methods[] = {{"a", "(J)J", (void *)(uintptr_t)1},
                               {"b", "(J)J", (void *)(uintptr_t)2},
                               {"c", "(J)J", (void *)(uintptr_t)3}};
  memory_failure = 1;
  assert(fh_register(&context, (jclass)(uintptr_t)7, methods, 3) == JNI_ERR &&
         exception_pending);
  assert(!strcmp(exception_class, "java/lang/OutOfMemoryError") &&
         !registration_weaks && !register_calls);
  memory_failure = exception_pending = 0;
  weak_failure = 1;
  assert(fh_register(&context, (jclass)(uintptr_t)7, methods, 3) == JNI_ERR);
  assert(!registration_weaks && exception_pending);
  weak_failure = exception_pending = 0;
  closure_failure = 1;
  assert(fh_register(&context, (jclass)(uintptr_t)7, methods, 3) == JNI_ERR);
  closure_failure = -1;
  assert(!register_calls && exception_pending);
  assert(!atomic_load(&counters.live_closures));
  exception_pending = 0;
  /* A preparation failure must release class ownership too. */
  context.module = 23;
  assert(fh_register(&context, (jclass)(uintptr_t)7, methods, 3) == JNI_OK);
  release_registrations(&env, 23);
  release_bindings(23);
  context.module = 22;
  assert(fh_register(&context, (jclass)(uintptr_t)7, methods, 3) == JNI_OK);
  assert(atomic_load(&counters.live_closures) == 3);
  registration_contract(methods);
  uncertain_unregister(methods);
  // A VM registration error may leave published entries. Retain them until
  // unregister/unload rather than freeing possible VM targets during rollback.
  reject_registration = 1;
  assert(fh_register(&context, (jclass)(uintptr_t)7, methods, 3) == JNI_ERR);
  assert(atomic_load(&counters.live_closures) == 3);
  reject_registration = exception_pending = 0;
  const size_t classes = 1024;
  for (size_t i = 1; i < classes; i++) {
    assert(fh_register(&context, (jclass)(uintptr_t)(7 + i), methods, 3) ==
           JNI_OK);
  }
  memory_failure = 1;
  assert(fh_register(&context, (jclass)(uintptr_t)9999, methods, 3) ==
             JNI_ERR &&
         exception_pending);
  assert(!strcmp(exception_class, "java/lang/OutOfMemoryError") &&
         registration_weaks == 1024);
  memory_failure = exception_pending = 0;
  context.module = 23;
  assert(fh_register(&context, (jclass)(uintptr_t)7, methods, 3) == JNI_ERR);
  context.module = 22;
  size_t retained_weaks = registration_weaks;
  local_failure = 1;
  assert(release_registrations(&env, 22) == JNI_ERR);
  assert(exception_pending && registration_weaks == retained_weaks);
  assert(atomic_load(&counters.live_closures) == 3);
  local_failure = exception_pending = 0;
  unregister_failure = 1;
  assert(release_registrations(&env, 22) == JNI_ERR);
  assert(registration_weaks == retained_weaks &&
         atomic_load(&counters.live_closures) == 3);
  unregister_failure = 0;
  assert(release_registrations(&env, 22) == JNI_OK);
  release_bindings(22);
  fh_bindings_finish();
  assert(!registration_weaks && !atomic_load(&counters.live_closures));
}

int main(void) {
  assert(!gw_binding_store_create(NULL));
  initial_pool();
  store_stress();
  store_failures();
  production_backend();
  assert(atomic_load(&counters.errors) ==
         3); /* Busy rejection, weak promotion OOM, unregister failure. */
  printf(
      "{\"result\":\"PASS\",\"backend\":\"%s\","
      "\"store_bindings\":8192,\"production_bindings\":%d,"
      "\"registered_classes\":%d,\"concurrent_calls\":10000,\"oom_cases\":40,"
      "\"injected_failures\":%zu,\"live_closures\":0,"
      "\"initial_capacity\":512,\"shared_modules\":64,\"initial_oom_cases\":24,"
      "\"outstanding_allocations\":0}\n",
      "zig", 4096, 1024, injected_failures);
  return 0;
}
