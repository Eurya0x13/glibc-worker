#define _GNU_SOURCE
#include "protocol.h"
#include "jni_proxy.h"
#include "full_bridge.h"
#include <dlfcn.h>
#include <errno.h>
#include <link.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
static struct shared *sh;
static __thread int marker = 73;

static uintptr_t tp(void) {
  return (uintptr_t)__builtin_thread_pointer();
}

static int callback(struct packet *p) {
  sh->packet = (uintptr_t)p;
  atomic_store_explicit(&sh->request, 1, memory_order_release);

  /* errno must remain the Guest EBADF while Host services the request. */
  while (!atomic_load_explicit(&sh->response, memory_order_acquire)) {
    __asm__ volatile("" ::: "memory");
  }

  return sh->reply;
}

static void *child(void *unused) {
  (void)unused;
  int good = marker == 73;
  marker = 94;
  errno = 47;
  sh->child_tp = tp();
  atomic_store_explicit(&sh->child_ready, 1, memory_order_release);

  while (!atomic_load_explicit(&sh->child_release, memory_order_acquire)) {
    __asm__ volatile("" ::: "memory");
  }
  sh->pthread_value = good && marker == 94 && errno == 47 ? 94 : -1;
  return (void *)(uintptr_t)sh->pthread_value;
}

static int visit(struct dl_phdr_info *info, size_t size, void *opaque) {
  (void)size;
  (void)opaque;

  if ((strstr(info->dlpi_name, "libc.so.6") ||
       strstr(info->dlpi_name, "libguest_libc.so"))) {
    sh->guest_libc_found = 1;
  }

  return 0;
}

int main(int argc, char **argv) {
  if (argc != 4) {
    return 91; /* Invalid invocation exits the disposable process. */
  }
  sh = (struct shared *)(uintptr_t)strtoull(argv[1], NULL, 16);
  if (!sh || sh->magic != MAGIC) {
    syscall(SYS_exit, 92);
  }

  full_guest_start(sh);
  sh->guest_pid = getpid();
  sh->guest_tid = (int)syscall(SYS_gettid);
  sh->guest_tp = tp();
  sh->guest_errno_ptr = (uintptr_t)&errno;
  atomic_store(&sh->phase, 1);
  sh->access_rc = access("/proc/self", F_OK);
  dl_iterate_phdr(visit, NULL);

  void *h = dlopen(argv[2], RTLD_NOW | RTLD_LOCAL);
  if (!h) {
    dprintf(2, "Guest dlopen failed: %s\n", dlerror());
    sh->probe_rc = -100;
    atomic_store(&sh->phase, -1);
    goto wait_stop;
  }
  probe_t f = (probe_t)dlsym(h, "guest_probe");
  if (!f) {
    sh->probe_rc = -101;
    atomic_store(&sh->phase, -1);
    goto wait_stop;
  }

  struct packet p = {.value = 37};
  sh->probe_rc = f(&p, callback);
  sh->guest_errno_after = errno;
  errno = 39;

  pthread_t t;
  sh->pthread_rc = pthread_create(&t, NULL, child, NULL);
  sh->errno_after_create = errno;
  if (!sh->pthread_rc) {
    /* errno after a successful libc call is not our isolation oracle.
       Observe independent values while both threads explicitly hold them. */
    errno = 39;
    while (!atomic_load_explicit(&sh->child_ready, memory_order_acquire)) {
      __asm__ volatile("" ::: "memory");
    }
    sh->thread_isolation_ok = marker == 73 && errno == 39;
    atomic_store_explicit(&sh->child_release, 1, memory_order_release);
    void *result = NULL;
    sh->pthread_rc = pthread_join(t, &result);
    sh->errno_after_join = errno;
    if (result != (void *)(uintptr_t)94) {
      sh->pthread_rc = -102;
    }
  }

  sh->guest_tls_after = marker;
  /* pthread child must not replace this thread's TLS/errno. */
  if (marker != 73 || !sh->thread_isolation_ok) {
    sh->pthread_rc = -103;
  }
  dlclose(h);

  sh->jni_status = -999;
  void *jh = dlopen(argv[3], RTLD_NOW | RTLD_LOCAL);
  if (!jh) {
    dprintf(2, "Guest JNI dlopen failed: %s\n", dlerror());
  } else {
    jint (*onload)(JavaVM *, void *) =
        (jint (*)(JavaVM *, void *))dlsym(jh, "JNI_OnLoad");
    jint (*status)(void) = (jint (*)(void))dlsym(jh, "guest_jni_status");
    if (onload && status) {
      JavaVM *vm = guest_jni_context_begin(sh);
      sh->jni_onload_version = onload(vm, NULL);
      sh->jni_status = status();
      guest_jni_context_end();
    }
    dlclose(jh);
  }
  atomic_store(&sh->phase, 2);

wait_stop:
  while (full_guest_wait(sh)) {
    atomic_fetch_add(&sh->heartbeat, 1);
  }

  /* Returning from main/exit/_exit would terminate the entire Host group. */
  full_guest_shutdown();
  syscall(SYS_exit, 0);
  __builtin_unreachable();
}
