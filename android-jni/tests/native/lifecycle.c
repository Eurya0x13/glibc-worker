/* Production Host lifecycle with a VM double. Each terminal case owns a PID;
   this does not execute a GNU Guest or establish Android/JRE compatibility. */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "host_bridge.h"
#include "host_internal.h"
#include "jni_vm_adapter.h"
#include "protocol.h"
#include "dlopen_backend.h"

static int mode, created, deleted;
static JavaVM target_vm;
static struct shared startup;
static const struct gw_host_bridge_config options = {
    .abi_version = GW_HOST_BRIDGE_CONFIG_V2,
    .struct_size = sizeof options,
    .native_directory = "unused",
    .image_catalog = "unused"};

void *__real_mmap(void *, size_t, int, int, int, off_t);

void *__wrap_mmap(void *p, size_t n, int prot, int flags, int fd, off_t off) {
  if (mode == 1) {
    errno = ENOMEM;
    return MAP_FAILED;
  }

  return __real_mmap(p, n, prot, flags, fd, off);
}

int __real_pthread_create(pthread_t *, const pthread_attr_t *,
                          void *(*)(void *), void *);

int __wrap_pthread_create(pthread_t *t, const pthread_attr_t *a,
                          void *(*fn)(void *), void *p) {
  return mode == 4 ? EAGAIN : __real_pthread_create(t, a, fn, p);
}

int __real_pthread_join(pthread_t, void **);

int __wrap_pthread_join(pthread_t t, void **p) {
  return mode == 5 ? EINVAL : __real_pthread_join(t, p);
}

static jint version(JNIEnv *env) {
  (void)env;
  return mode == 2 ? JNI_VERSION_1_4 : 0x00010008;
}

static jint get_vm(JNIEnv *env, JavaVM **vm) {
  (void)env;
  *vm = &target_vm;
  return JNI_OK;
}

static jclass find_class(JNIEnv *env, const char *name) {
  (void)env;
  (void)name;
  return (jclass)(uintptr_t)1;
}

static jobject new_global(JNIEnv *env, jobject ref) {
  (void)env;
  created++;
  return ref;
}

static void delete_global(JNIEnv *env, jobject ref) {
  (void)env;
  (void)ref;
  deleted++;
}

static void delete_local(JNIEnv *env, jobject ref) {
  (void)env;
  (void)ref;
}

static jmethodID get_method(JNIEnv *env, jclass cls, const char *name,
                            const char *sig) {
  (void)env;
  (void)cls;
  (void)name;
  (void)sig;

  return (jmethodID)(uintptr_t)1;
}

static jstring new_string(JNIEnv *env, const char *s) {
  (void)env;
  return (jstring)s;
}

static void call_void(JNIEnv *env, jclass cls, jmethodID method, ...) {
  (void)env;
  (void)cls;
  (void)method;
}

static jboolean exception_check(JNIEnv *env) {
  (void)env;
  return JNI_FALSE;
}

static jint throw_new(JNIEnv *env, jclass cls, const char *message) {
  (void)env;
  (void)cls;
  (void)message;
  return JNI_OK;
}
static const struct JNINativeInterface vm = {.GetVersion = version,
                                             .GetJavaVM = get_vm,
                                             .FindClass = find_class,
                                             .NewGlobalRef = new_global,
                                             .DeleteGlobalRef = delete_global,
                                             .DeleteLocalRef = delete_local,
                                             .GetStaticMethodID = get_method,
                                             .NewStringUTF = new_string,
                                             .CallStaticVoidMethod = call_void,
                                             .ExceptionCheck = exception_check,
                                             .ThrowNew = throw_new};
static JNIEnv env = &vm;

static int install(JNIEnv *e, const char *dir, const char *path,
                   struct gw_loader_error *error) {
  (void)e;
  (void)dir;
  (void)path;

  return mode == 3 ? gw_dlopen_fail(error, GW_E_BRIDGE, "install failed")
                   : GW_OK;
}

static int restore(JNIEnv *e, struct gw_loader_error *error) {
  (void)e;
  return gw_dlopen_fail(error, GW_E_BRIDGE, "restore failed");
}
static const struct gw_jni_vm_adapter_v1 adapter = {
    GW_JNI_VM_ADAPTER_V1, sizeof adapter, install, restore};

static void *foreign_lifecycle(void *unused) {
  (void)unused;

  assert(full_host_state() == GW_HOST_STARTED);
  assert(full_host_ready() == JNI_ERR);
  assert(full_host_shutdown() == JNI_ERR);
  assert(!gw_host_current);

  return NULL;
}

static pthread_barrier_t start_barrier;
static _Atomic int winners;

static void *race_start(void *unused) {
  (void)unused;
  int rc = pthread_barrier_wait(&start_barrier);
  assert(!rc || rc == PTHREAD_BARRIER_SERIAL_THREAD);

  if (full_host_start(&startup, &env, &options) == JNI_OK) {
    atomic_fetch_add(&winners, 1);
    struct fb_call *hello = &gw_host_bridge->ctx[0].call[0];
    hello->args[12].j = FB_JNI_ACK;
    assert(fb_loader_acknowledge(hello));
    atomic_store(&startup.phase, 2);
    assert(full_host_ready() == JNI_OK);
    assert(full_host_shutdown() == JNI_OK);
  }

  return NULL;
}

static void run_case(void) {
  assert(full_host_state() == GW_HOST_NEW);

  if (mode == 8) {
    pthread_t threads[2];
    assert(!pthread_barrier_init(&start_barrier, NULL, 2));
    for (int i = 0; i < 2; i++) {
      assert(!pthread_create(&threads[i], NULL, race_start, NULL));
    }
    for (int i = 0; i < 2; i++) {
      assert(!pthread_join(threads[i], NULL));
    }
    assert(!pthread_barrier_destroy(&start_barrier));
    assert(atomic_load(&winners) == 1 && full_host_state() == GW_HOST_STOPPED);
    assert(created == 2 && deleted == created);
    return;
  }

  assert(full_host_shutdown() == JNI_ERR && full_host_ready() == JNI_ERR);
  assert(full_host_start(NULL, &env, &options) == JNI_ERR);
  assert(full_host_state() == GW_HOST_NEW);

  struct gw_host_bridge_config selected = options;
  if (mode == 3 || mode == 6) {
    assert(gw_jni_register_vm_adapter(&adapter) == GW_OK);
    selected.flags = GW_HOST_VM_ADAPTER;
  }
  int started = full_host_start(&startup, &env, &selected);
  if (mode >= 1 && mode <= 4) {
    assert(started == JNI_ERR);
  } else {
    assert(started == JNI_OK && full_host_state() == GW_HOST_STARTED);
    if (!mode) {
      pthread_t t;
      assert(!pthread_create(&t, NULL, foreign_lifecycle, NULL));
      assert(!pthread_join(t, NULL));
      assert(full_host_start(&startup, &env, &selected) == JNI_ERR);
    }

    struct fb_call *hello = &gw_host_bridge->ctx[0].call[0];
    if (mode != 7) {
      hello->args[12].j = FB_JNI_ACK;
      assert(fb_loader_acknowledge(hello));
      atomic_store(&startup.phase, 2);
    }

    int ready = full_host_ready();
    if (mode == 7) {
      assert(ready == JNI_ERR);
    } else {
      assert(ready == JNI_OK && full_host_state() == GW_HOST_READY);
      assert(full_host_ready() == JNI_ERR);
      if (!mode) {
        atomic_store(&gw_host_bridge->ctx[1].state, FB_RUNNING);
        atomic_store(&gw_host_bridge->ctx[1].call[0].request, 1);
        assert(full_host_shutdown() == JNI_ERR);
        atomic_store(&gw_host_bridge->ctx[1].call[0].response, 1);
        atomic_store(&gw_host_bridge->ctx[1].rpc[0].active, 1);
        assert(full_host_shutdown() == JNI_ERR);
        atomic_store(&gw_host_bridge->ctx[1].rpc[0].active, 0);
        assert(fb_ensure_frame(&gw_host_bridge->ctx[1], FB_DEPTH + 1));
        struct fb_call *deep = fb_call_at(&gw_host_bridge->ctx[1], FB_DEPTH);
        atomic_store(&deep->request, 1);
        assert(full_host_shutdown() == JNI_ERR);
        atomic_store(&deep->response, 1);
        atomic_store(&gw_host_bridge->ctx[1].emergency.request, 1);
        assert(full_host_shutdown() == JNI_ERR);
        atomic_store(&gw_host_bridge->ctx[1].emergency.response, 1);
        atomic_store(&gw_host_bridge->ctx[1].origin, 1);
        assert(full_host_shutdown() == JNI_ERR);
        atomic_store(&gw_host_bridge->ctx[1].origin, 0);
        atomic_store(&gw_host_bridge->ctx[1].state, FB_JOINED);
        assert(full_host_shutdown() == JNI_ERR);
        atomic_store(&gw_host_bridge->ctx[1].state, FB_RUNNING);
        gw_host_current->depth = 1;
        assert(full_host_shutdown() == JNI_ERR);
        gw_host_current->depth = 0;
        assert(full_host_state() == GW_HOST_READY && !deleted);
      }
      int stopped = full_host_shutdown();
      assert(stopped == (mode ? JNI_ERR : JNI_OK));
    }
  }

  if (mode) {
    assert(full_host_state() == GW_HOST_QUARANTINED);
    /* Image policy is released before the injected join failure, after all
       callers are quiescent. The helper reference remains retained. */
    assert(deleted == (mode == 5 ? 1 : 0));
  } else {
    assert(full_host_state() == GW_HOST_STOPPED && !gw_host_current);
    assert(created == 2 && deleted == created);
    assert(gw_host_bridge); /* Published mapping intentionally retained. */
  }

  assert(full_host_shutdown() == JNI_ERR && full_host_ready() == JNI_ERR);
  assert(full_host_start(&startup, &env, &selected) == JNI_ERR);
  assert(gw_jni_register_vm_adapter(&adapter) == GW_ERROR);
}

int main(void) {
  for (mode = 0; mode < 9; mode++) {
    pid_t pid = fork();
    assert(pid >= 0);
    if (!pid) {
      run_case();
      _exit(0);
    }
    int status;
    assert(waitpid(pid, &status, 0) == pid);
    if (!WIFEXITED(status) || WEXITSTATUS(status)) {
      fprintf(stderr, "lifecycle case %d failed\n", mode);
      return 1;
    }
  }

  puts(
      "{\"result\":\"PASS\",\"scenarios\":9,\"singleton\":true,"
      "\"concurrent_start_excluded\":true,"
      "\"owner_thread\":true,\"terminal_shutdown\":true,"
      "\"startup_failure_retained\":true,\"unsafe_shutdown_retained\":true}");
}
