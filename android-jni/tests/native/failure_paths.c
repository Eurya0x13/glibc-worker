/* Production Host/Guest RPC with a VM double. No Android/TLS claim. */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host_internal.h"
#include "guest_internal.h"
#include "protocol.h"

static struct full_bridge bridge;
static struct host_context service_context;
static _Atomic int stop_server, mode;
static int pending, last_class, globals_created, globals_deleted, weaks_created,
    weaks_deleted;
static _Atomic int fail_memory, fail_tls, tls_scope, fail_pages, fail_private;
void *__real_calloc(size_t, size_t);

void *__wrap_calloc(size_t count, size_t size) {
  return atomic_load(&fail_private) ? NULL : __real_calloc(count, size);
}

void *__real_mmap(void *, size_t, int, int, int, off_t);

void *__wrap_mmap(void *p, size_t n, int prot, int flags, int fd, off_t off) {
  if (atomic_load(&fail_pages)) {
    errno = ENOMEM;
    return MAP_FAILED;
  }

  return __real_mmap(p, n, prot, flags, fd, off);
}

static unsigned checks;
#define CHECK(x) \
  do {           \
    assert(x);   \
    checks++;    \
  } while (0)

int __real_posix_memalign(void **, size_t, size_t);

int __wrap_posix_memalign(void **p, size_t alignment, size_t size) {
  return atomic_load(&fail_memory) ? ENOMEM
                                   : __real_posix_memalign(p, alignment, size);
}

int __real_pthread_setspecific(pthread_key_t, const void *);

int __wrap_pthread_setspecific(pthread_key_t key, const void *value) {
  if (!atomic_load(&tls_scope)) {
    return __real_pthread_setspecific(key, value);
  }
  return atomic_load(&fail_tls) ? ENOMEM : 0;
}

static jboolean has_exception(JNIEnv *e) {
  (void)e;
  return pending != 0;
}

static void clear_exception(JNIEnv *e) {
  (void)e;
  pending = 0;
}

static jthrowable exception(JNIEnv *e) {
  (void)e;
  return (jthrowable)(uintptr_t)pending;
}

static jclass find_class(JNIEnv *e, const char *name) {
  (void)e;
  last_class = !strcmp(name, "java/lang/OutOfMemoryError") ? FB_FAILURE_MEMORY
               : !strcmp(name, "java/lang/StackOverflowError")
                   ? FB_FAILURE_DEPTH
               : !strcmp(name, "java/lang/IllegalArgumentException")
                   ? FB_FAILURE_ARGUMENT
                   : FB_FAILURE_STATE;

  return (jclass)(uintptr_t)last_class;
}

static jint throw_new(JNIEnv *e, jclass cls, const char *text) {
  (void)e;
  (void)text;
  pending = (int)(uintptr_t)cls;
  return JNI_OK;
}

static void delete_local(JNIEnv *e, jobject v) {
  (void)e;
  (void)v;
}

static jobject new_global(JNIEnv *e, jobject v) {
  (void)e;
  globals_created++;
  return v;
}

static void delete_global(JNIEnv *e, jobject v) {
  (void)e;
  (void)v;
  globals_deleted++;
}

static jweak new_weak(JNIEnv *e, jobject v) {
  (void)e;
  weaks_created++;
  return (jweak)v;
}

static void delete_weak(JNIEnv *e, jweak v) {
  (void)e;
  (void)v;
  weaks_deleted++;
}

static jboolean same_object(JNIEnv *e, jobject a, jobject b) {
  (void)e;
  return a == b;
}
static const struct JNINativeInterface vm = {.ExceptionCheck = has_exception,
                                             .IsSameObject = same_object,
                                             .ExceptionClear = clear_exception,
                                             .ExceptionOccurred = exception,
                                             .FindClass = find_class,
                                             .ThrowNew = throw_new,
                                             .DeleteLocalRef = delete_local,
                                             .NewGlobalRef = new_global,
                                             .NewWeakGlobalRef = new_weak,
                                             .DeleteWeakGlobalRef = delete_weak,
                                             .DeleteGlobalRef = delete_global};
static JNIEnv environment = &vm;

static void *server(void *unused) {
  (void)unused;
  gw_host_current = &service_context;

  while (!atomic_load(&stop_server)) {
    int action = atomic_load(&mode);
    struct fb_context *w = &bridge.ctx[1];
    int state = atomic_load_explicit(&w->state, memory_order_acquire);

    if (action == 1 && state == FB_GUEST_ATTACH) {
      w->attach_result = JNI_ENOMEM;
      atomic_store_explicit(&w->state, FB_ATTACH_FAILED, memory_order_release);
      fb_event_notify(&w->guest_event);
    } else if (action == 2 && state == FB_HOST_REQUEST) {
      w->attach_result = JNI_ENOMEM;
      atomic_store_explicit(&w->state, FB_ATTACH_FAILED, memory_order_release);
      fb_event_notify(&w->host_event);
    } else if (action == 3 && state == FB_HOST_REQUEST) {
      atomic_store_explicit(&w->state, FB_RUNNING, memory_order_release);
      fb_event_notify(&w->host_event);
    }

    for (int i = -1; i < fb_frame_capacity(&bridge.ctx[0]); i++) {
      struct fb_rpc *r =
          i < 0 ? &bridge.ctx[0].emergency : fb_rpc_at(&bridge.ctx[0], i);
      if (atomic_load_explicit(&r->request, memory_order_acquire) !=
          atomic_load(&r->response)) {
        fh_service(&service_context, r);
      }
    }
    sched_yield();
  }
  return NULL;
}

int main(void) {
  gw_host_bridge = &bridge;
  atomic_store(&bridge.running, 1);
  gw_host_jni_version = 0x00010008;
  service_context = (struct host_context){.env = &environment, .id = 0};
  struct shared startup = {.full_bridge = (uintptr_t)&bridge};
  struct fb_call *hello = &bridge.ctx[0].call[0];
  fb_loader_offer(hello);
  hello->args[10].j = FB_JNI_HELLO;
  hello->args[11].i = 0x00010008;
  full_guest_start(&startup);
  pthread_t worker;
  assert(!pthread_create(&worker, NULL, server, NULL));
  JNIEnv *guest = &fg_env;
  CHECK((*guest)->GetVersion(guest) == 0x00010008);

  /* Failed frame growth uses an allocation-independent emergency RPC. */
  gdepth = FB_DEPTH;
  atomic_store(&fail_pages, 1);
  CHECK((*guest)->RegisterNatives(guest, NULL, NULL, 0) == JNI_ERR);
  CHECK((*guest)->ExceptionCheck(guest) && pending == FB_FAILURE_MEMORY);
  jthrowable token = (*guest)->ExceptionOccurred(guest);
  CHECK(token);
  (*guest)->ExceptionClear(guest);
  (*guest)->DeleteLocalRef(guest, token);
  CHECK(!(*guest)->ExceptionCheck(guest));
  gdepth = 0;
  atomic_store(&fail_pages, 0);
  CHECK((*guest)->GetVersion(guest) == 0x00010008);

  /* Fill the reserve, fail growth after VM NewGlobalRef, and verify rollback.
   */
  const size_t count = 16384;
  jobject *tokens = calloc(count, sizeof *tokens);
  assert(tokens);
  for (size_t i = 0; i < count; i++) {
    tokens[i] =
        fh_add_ref(&service_context, (jobject)(uintptr_t)(i + 100), H_GLOBAL);
    assert(tokens[i]);
  }
  atomic_store(&fail_memory, 1);
  CHECK(!(*guest)->NewGlobalRef(guest, tokens[0]));
  atomic_store(&fail_memory, 0);
  CHECK((*guest)->ExceptionCheck(guest) && pending == FB_FAILURE_MEMORY);
  (*guest)->ExceptionClear(guest);
  CHECK(globals_created == 1 && globals_deleted == 1 &&
        atomic_load(&bridge.live_global) == (int)count);
  atomic_store(&fail_memory, 1);
  CHECK(!fh_add_id(&service_context, (void *)(uintptr_t)77, H_METHOD, "()I",
                   (jclass)(uintptr_t)88));
  atomic_store(&fail_memory, 0);
  CHECK(pending == FB_FAILURE_MEMORY && weaks_created == 1 &&
        weaks_deleted == 1 && !atomic_load(&bridge.id_cache_weak));
  pending = 0;
  for (size_t i = 0; i < count; i++) {
    CHECK(fh_ref(&service_context, tokens[i]) == (jobject)(uintptr_t)(i + 100));
    fh_drop_token(tokens[i]);
  }
  free(tokens);
  sweep_ids(&environment, 1);

  /* A free handle slot is not enough: index growth can fail independently.
     Preserve the pending exception and roll back both the token and weak. */
  jobject anchor =
      fh_add_ref(&service_context, (jobject)(uintptr_t)99, H_GLOBAL);
  CHECK(anchor);
  int weak_created_before = weaks_created, weak_deleted_before = weaks_deleted;
  pending = FB_FAILURE_ARGUMENT;
  atomic_store(&fail_memory, 1);
  CHECK(!fh_add_id(&service_context, (void *)(uintptr_t)77, H_METHOD, "()I",
                   (jclass)(uintptr_t)88));
  atomic_store(&fail_memory, 0);
  CHECK(pending == FB_FAILURE_ARGUMENT &&
        weaks_created == weak_created_before + 1 &&
        weaks_deleted == weak_deleted_before + 1 &&
        !atomic_load(&bridge.id_cache_weak));
  pending = 0;
  jobject recovered_id = fh_add_id(&service_context, (void *)(uintptr_t)77,
                                   H_METHOD, "()I", (jclass)(uintptr_t)88);
  CHECK(recovered_id && atomic_load(&bridge.id_cache_weak) == 1);
  CHECK(fh_add_id(&service_context, (void *)(uintptr_t)77, H_METHOD, "()I",
                  (jclass)(uintptr_t)88) == recovered_id);
  sweep_ids(&environment, 1);
  CHECK(!atomic_load(&bridge.id_cache_weak) &&
        fh_ref(&service_context, anchor) == (jobject)(uintptr_t)99);
  fh_drop_token(anchor);
  sweep_ids(&environment, 1);

  pending = FB_FAILURE_ARGUMENT;
  fh_throw(&environment, FB_FAILURE_MEMORY, "must preserve original");
  CHECK(pending == FB_FAILURE_ARGUMENT);
  pending = 0;
  atomic_store(&tls_scope, 1);
  for (int i = 1; i < FB_CONTEXTS; i++) {
    atomic_store(&bridge.ctx[i].state, FB_RUNNING);
  }
  struct gw_loader_error error = {0};
  atomic_store(&fail_pages, 1);
  CHECK(!fh_context(&environment, &error) && error.code == GW_E_CAPACITY &&
        pending == FB_FAILURE_MEMORY);
  pending = 0;
  atomic_store(&fail_pages, 0);
  atomic_store(&bridge.ctx[1].state, FB_FREE);
  atomic_store(&fail_private, 1);
  CHECK(!fh_context(&environment, &error) && error.code == GW_E_CAPACITY &&
        pending == FB_FAILURE_MEMORY &&
        atomic_load(&bridge.ctx[1].state) == FB_FREE);
  pending = 0;
  atomic_store(&fail_private, 0);
  atomic_store(&fail_tls, 1);
  CHECK(!fh_context(&environment, &error) && error.code == GW_E_CAPACITY &&
        pending == FB_FAILURE_MEMORY);
  CHECK(!gw_host_current && atomic_load(&bridge.ctx[1].state) == FB_FREE);
  pending = 0;
  atomic_store(&fail_tls, 0);
  atomic_store(&mode, 2);
  CHECK(!fh_context(&environment, &error) && error.code == GW_E_CAPACITY &&
        pending == FB_FAILURE_MEMORY);
  CHECK(!gw_host_current && atomic_load(&bridge.ctx[1].state) == FB_FREE);
  pending = 0;

  atomic_store(&mode, 1);
  gid = -1;
  JNIEnv *attached = (void *)(uintptr_t)1;
  atomic_store(&fail_private, 1);
  CHECK(fg_vm->AttachCurrentThread(&fg_vm, &attached, NULL) == JNI_ENOMEM &&
        !attached && gid == -1 && atomic_load(&bridge.ctx[1].state) == FB_FREE);
  atomic_store(&fail_private, 0);
  CHECK(fg_vm->AttachCurrentThread(&fg_vm, &attached, NULL) == JNI_ENOMEM &&
        !attached && gid == -1);
  atomic_store(&mode, 0);
  atomic_store(&bridge.ctx[1].state, FB_FREE);
  gid = 0;
  atomic_store(&mode, 3);
  CHECK(fh_context(&environment, &error));
  CHECK(gw_host_current && gw_host_current->id == 1);
  gw_host_current = NULL;
  atomic_store(&bridge.ctx[1].state, FB_FREE);
  atomic_store(&mode, 0);

  atomic_store(&tls_scope, 0);
  atomic_store(&stop_server, 1);
  assert(!pthread_join(worker, NULL));
  CHECK(!atomic_load(&bridge.live_local) && !atomic_load(&bridge.live_global));
  printf(
      "{\"result\":\"PASS\",\"checks\":%u,\"depth_recovery\":true,\"oom_"
      "rollback\":true,\"id_index_oom_rollback\":true,\"context_status\":true}"
      "\n",
      checks);
}
