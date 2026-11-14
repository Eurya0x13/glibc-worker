#define _GNU_SOURCE
#include "protocol.h"
#include "full_bridge.h"
#include "lab_suite.h"
#include "loader_image.h"
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#if GW_RPC_PROFILE
/* This harness is a separate DSO. Header-only event instrumentation uses its
   own unassigned TLS, never the runtime DSO's private diagnostic state. */
_Thread_local struct gw_rpc_row *gw_rpc_current
    __attribute__((visibility("hidden")));
_Thread_local struct gw_rpc_scope *gw_rpc_scope_current
    __attribute__((visibility("hidden")));
#endif
extern int app_host_callback(int value);
extern void app_host_jni_dispatch(struct shared *s);
extern JNIEnv *app_host_jni_env(void);
extern long launch_guest(unsigned long flags, void *sp, void *entry);
static __thread int host_marker = 61;

static void *host_child(void *arg) {
  (void)arg;
  int initial = host_marker;
  host_marker = 62;
  errno = 48;
  return (void *)(uintptr_t)(initial == 61 && host_marker == 62 && errno == 48);
}

static void die(const char *msg) {
  perror(msg);
  exit(1);
}

static void require(int ok, const char *msg) {
  if (!ok) {
    fprintf(stderr, "FAIL: %s\n", msg);
    exit(1);
  }
}

static long now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

int worker_host_main(int argc, char **argv) {
  require(argc == 7,
          "usage: bootstrap ld.so library-dir worker guest-dso guest-jni-dso "
          "jre-home");
  setbuf(stdout, NULL);
  _Static_assert(ATOMIC_INT_LOCK_FREE == 2,
                 "protocol needs lock-free atomic int");

  struct shared *s = mmap(NULL, sizeof *s, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (s == MAP_FAILED) {
    die("shared mmap");
  }
  s->magic = MAGIC;
  s->host_pid = getpid();
  s->host_tid = (int)syscall(SYS_gettid);
  s->access_rc = -999;

  char catalog[PATH_MAX];
  require(snprintf(catalog, sizeof catalog, "%s/guest-images.tsv", argv[6]) <
              (int)sizeof catalog,
          "lab image policy paths");
  struct gw_host_bridge_config config = {
      .abi_version = GW_HOST_BRIDGE_CONFIG_V1,
      .struct_size = sizeof config,
      .flags = GW_HOST_LEGACY_JRE17,
      .native_directory = argv[2],
      .image_catalog = catalog};
  char copy_marker[PATH_MAX];
  require(
      snprintf(copy_marker, sizeof copy_marker, "%s/array-copy-cache-enabled",
               argv[6]) < (int)sizeof copy_marker,
      "array cache marker path");
  if (!access(copy_marker, F_OK)) {
    config.flags |= GW_HOST_ARRAY_COPY_CACHE;
  }
  require(
      snprintf(copy_marker, sizeof copy_marker, "%s/array-copy-cache-disabled",
               argv[6]) < (int)sizeof copy_marker,
      "array cache disable marker path");
  if (!access(copy_marker, F_OK)) {
    config.flags |= GW_HOST_DISABLE_ARRAY_COPY_CACHE;
  }
  require(full_host_start(s, app_host_jni_env(), &config) == JNI_OK,
          "Host bridge and loader configuration");
  struct full_bridge *bridge = (struct full_bridge *)s->full_bridge;
  snprintf(bridge->module_path, sizeof bridge->module_path,
           "%s/libguest_conformance.so", argv[2]);
  int smoke_only = strcmp(argv[0], "smoke") == 0;
  if (!smoke_only) {
    require(gw_lab_prepare(bridge, app_host_jni_env(), argv[2]) == 0,
            "coverage diagnostics and preflight");
  }

  uintptr_t host_tp = (uintptr_t)__builtin_thread_pointer(),
            host_errno_ptr = (uintptr_t)&errno;
  struct gw_bootstrap_image l = gw_bootstrap_map(argv[1]);
  size_t stack_size = 8 * 1024 * 1024;
  void *stack = mmap(NULL, stack_size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
  if (stack == MAP_FAILED) {
    die("stack mmap");
  }
  require(!mprotect(stack, 4096, PROT_NONE), "stack guard");
  char address[32];
  snprintf(address, sizeof address, "%llx", (unsigned long long)(uintptr_t)s);
  char preload[8 * PATH_MAX + 256];
  snprintf(preload, sizeof preload,
           "%s/libguest_libc.so:%s/libguest_pthread.so", argv[2], argv[2]);
  static const char *extra_preloads[] = {
#include "generated/compat-preload.inc"
      NULL};
  size_t used = strlen(preload);
  for (size_t i = 0; extra_preloads[i]; i++) {
    int added = snprintf(preload + used, sizeof preload - used, ":%s/%s",
                         argv[2], extra_preloads[i]);
    require(added > 0 && (size_t)added < sizeof preload - used,
            "Guest dependency preload bounds");
    used += (size_t)added;
  }
  char *args[] = {argv[1], "--library-path", argv[2], "--preload", preload,
                  argv[3], address,          argv[4], argv[5]};
  void *sp = gw_bootstrap_stack(l, stack, stack_size, args, 9);

  printf("Host pid=%d tid=%d tp=%llx errno_ptr=%llx\n", s->host_pid,
         s->host_tid, (unsigned long long)host_tp,
         (unsigned long long)host_errno_ptr);
  printf("Guest ld.so base=%p entry=%p initial_sp=%p\n", l.base, l.entry, sp);
  /* Guest inherits a blocked signal mask. Signal coexistence is NOT tested. */
  sigset_t all, old;
  sigfillset(&all);
  sigdelset(&all, SIGSYS);
  sigprocmask(SIG_SETMASK, &all, &old);

  errno = 123;
  long tid = launch_guest(CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
                              CLONE_THREAD | CLONE_SYSVSEM,
                          sp, l.entry);
  sigprocmask(SIG_SETMASK, &old, NULL);
  require(tid > 0, "raw clone accepted");
  printf("raw clone Guest tid=%ld (same thread group)\n", tid);

  long deadline = now_ms() + 5000;
  unsigned host_ticks = 0;
  int callback_ok = 0;
  int allocator_ok = 1;
  while (now_ms() < deadline) {
    require(errno == 123 && host_marker == 61 &&
                (uintptr_t)__builtin_thread_pointer() == host_tp,
            "Host TLS/errno intact during Guest execution");
    if (atomic_load_explicit(&s->request, memory_order_acquire) &&
        !atomic_load(&s->response)) {
      struct packet *p = (struct packet *)s->packet;
      callback_ok = p && p->value == 42;
      s->reply = callback_ok ? app_host_callback(p->value) : -84;
      atomic_store_explicit(&s->response, 1, memory_order_release);
    }
    app_host_jni_dispatch(s);
    full_host_poll();
    host_ticks++;
    int saved_errno = errno;
    volatile unsigned char *block = malloc(4096);
    if (!block) {
      allocator_ok = 0;
    } else {
      block[0] = 0x5a;
      block[4095] = 0xa5;
      allocator_ok &= block[0] == 0x5a && block[4095] == 0xa5;
      free((void *)block);
    }
    errno = saved_errno; /* Host's own successful allocator may change errno. */
    int phase = atomic_load(&s->phase);
    if (phase == 2 || phase == -1) {
      break;
    }
    usleep(1000);
  }

  int phase = atomic_load(&s->phase);
  if (phase != 2 && phase != -1) {
    fprintf(
        stderr,
        "FAIL startup deadline phase=%d; disposing entire experimental Host\n",
        phase);
    return 1;
  }
  int full_rc = 1;
  if (phase == 2 && full_host_ready() == JNI_OK) {
    full_rc = smoke_only ? 0 : full_host_suite(s);
  }

  printf("phase=%d Guest pid=%d tid=%d tp=%llx errno_ptr=%llx\n", phase,
         s->guest_pid, s->guest_tid, (unsigned long long)s->guest_tp,
         (unsigned long long)s->guest_errno_ptr);
  printf(
      "probe=%d callback_ok=%d guest_errno=%d guest_tls=%d pthread=%d "
      "child_value=%d "
      "child_tp=%llx libc_found=%d\n",
      s->probe_rc, callback_ok, s->guest_errno_after, s->guest_tls_after,
      s->pthread_rc, s->pthread_value, (unsigned long long)s->child_tp,
      s->guest_libc_found);
  printf(
      "Guest errno after pthread_create=%d after pthread_join=%d (before=39, "
      "child=47)\n",
      s->errno_after_create, s->errno_after_join);
  printf("Guest live parent/child errno+TLS isolation=%d Host allocator=%d\n",
         s->thread_isolation_ok, allocator_ok);
  printf("Guest libc access rc=%d\n", s->access_rc);
  int jni_ok = s->jni_onload_version == 0x00010006 && s->jni_status == 0 &&
               !s->jni_bridge_error && s->jni_requests == 15;
  printf(
      "GUEST_JNI_RESULT=%s onload=0x%x status=%d bridge_error=%d requests=%d "
      "guest_tid=%d "
      "host_tid=%d\n",
      jni_ok ? "PASS" : "FAIL", s->jni_onload_version, s->jni_status,
      s->jni_bridge_error, s->jni_requests, s->guest_tid, s->host_tid);

  unsigned h0 = atomic_load(&s->heartbeat);
  /* Liveness is an explicit wake/ack, not a requirement to spin while idle. */
  fb_event_notify(&bridge->ctx[0].guest_event);
  usleep(20000);
  unsigned h1 = atomic_load(&s->heartbeat);
  printf("Host ticks=%u Guest heartbeat=%u -> %u\n", host_ticks, h0, h1);

  pthread_t host_thread;
  void *host_result = NULL;
  int host_pthread = pthread_create(&host_thread, NULL, host_child, NULL);
  if (!host_pthread) {
    host_pthread = pthread_join(host_thread, &host_result);
  }
  int host_thread_ok =
      !host_pthread && host_result == (void *)(uintptr_t)1 && host_marker == 61;
  printf("Host pthread after Guest bootstrap=%d main TLS=%d\n", host_thread_ok,
         host_marker);

  FILE *maps = fopen("/proc/self/maps", "r");
  if (maps) {
    char line[1024];
    while (fgets(line, sizeof line, maps)) {
      if (strstr(line, "libc.so") || strstr(line, "linker64") ||
          strstr(line, "guest_worker") || strstr(line, "libart.so") ||
          strstr(line, "libjvm.so")) {
        printf("MAP %s", line);
      }
    }
    fclose(maps);
  }

  int good = phase == 2 && s->guest_pid == s->host_pid &&
             s->guest_tid == (int)tid && s->guest_tid != s->host_tid &&
             s->guest_tp != host_tp && s->guest_errno_ptr != host_errno_ptr &&
             s->probe_rc == 0 && callback_ok && s->guest_errno_after == EBADF &&
             s->guest_tls_after == 73 && s->pthread_rc == 0 &&
             s->pthread_value == 94 && s->child_tp != s->guest_tp &&
             s->thread_isolation_ok && s->access_rc == 0 && allocator_ok &&
             host_thread_ok && s->guest_libc_found && h1 > h0 && jni_ok &&
             !full_rc;

  if (full_host_shutdown() != JNI_OK) {
    return 1;
  }
  atomic_store(&s->stop, 1);
  char task[128];
  snprintf(task, sizeof task, "/proc/self/task/%ld", tid);
  deadline = now_ms() + 2000;
  while (access(task, F_OK) == 0 && now_ms() < deadline) {
    usleep(1000);
  }
  int gone = access(task, F_OK) != 0;
  printf("Guest raw thread exit observed=%d Host still alive pid=%d\n", gone,
         getpid());
  printf("SMOKE_TEST_RESULT=%s group=%s\n", good && gone ? "PASS" : "FAIL",
         smoke_only ? "smoke" : "coverage");
  /* Mappings deliberately live until disposable Host process exit. */
  return good && gone ? 0 : 1;
}
