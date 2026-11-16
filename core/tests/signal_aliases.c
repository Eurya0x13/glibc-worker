#define _GNU_SOURCE
#include "signal-router.h"
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

typedef __sighandler_t (*registration)(int, __sighandler_t);
typedef int (*action_fn)(int, const struct sigaction *, struct sigaction *);

enum { TEST_SIGNAL = 50, MAX_SNAPSHOTS = 32 };

struct observation {
  uint64_t mask;
  unsigned flags;
  __sighandler_t handler;
};

struct trace {
  struct observation actions[MAX_SNAPSHOTS];
  unsigned count, registrations;
};

static volatile sig_atomic_t deliveries;

static void handler(int number) {
  assert(number == TEST_SIGNAL);
  ++deliveries;
}

static void *lookup(void *library, const char *name) {
  void *entry = dlsym(library, name);
  assert(entry);
  return entry;
}

static void observe(struct trace *trace, action_fn action, int routed) {
  struct sigaction current;
  assert(!action(TEST_SIGNAL, NULL, &current));
  assert(trace->count < MAX_SNAPSHOTS);
  struct observation *record = &trace->actions[trace->count++];
  memcpy(&record->mask, &current.sa_mask, sizeof record->mask);
  /* SA_RESTORER is libc's kernel ABI detail, not a logical registration. */
  record->flags = current.sa_flags & (SA_RESTART | SA_NODEFER | SA_RESETHAND |
                                      SA_SIGINFO | SA_ONSTACK);
  record->handler = current.sa_handler;
  if (routed) {
    assert(!signal_router_verify());
  }
}

static void exercise(void *library, action_fn action, int routed,
                     struct trace *trace) {
  const char *names[] = {
      "signal",
#ifdef __GLIBC__
      "bsd_signal", "ssignal", "sysv_signal", "__sysv_signal",
#endif
  };
  struct sigaction reset = {.sa_handler = SIG_DFL};
  sigemptyset(&reset.sa_mask);
  for (unsigned i = 0; i < sizeof names / sizeof names[0]; ++i) {
    registration entry = lookup(library, names[i]);
    assert(!action(TEST_SIGNAL, &reset, NULL));
    assert(entry(TEST_SIGNAL, handler) == SIG_DFL);
    observe(trace, action, routed);
    int before = deliveries;
    assert(!raise(TEST_SIGNAL) && deliveries == before + 1);
    observe(trace, action, routed);
    ++trace->registrations;
  }
  registration set = lookup(library, "sigset");
  registration bsd = lookup(library, "signal");
  int (*interrupt)(int, int) = lookup(library, "siginterrupt");
  int (*ignore)(int) = lookup(library, "sigignore");
  assert(!action(TEST_SIGNAL, &reset, NULL));
  assert(bsd(TEST_SIGNAL, handler) == SIG_DFL);
  assert(!interrupt(TEST_SIGNAL, 1));
  observe(trace, action, routed);
  assert(bsd(TEST_SIGNAL, handler) == handler);
  observe(trace, action, routed);
  assert(!interrupt(TEST_SIGNAL, 0));
  observe(trace, action, routed);
  assert(!ignore(TEST_SIGNAL));
  observe(trace, action, routed);
  assert(set(TEST_SIGNAL, handler) == SIG_IGN);
  observe(trace, action, routed);
  assert(set(TEST_SIGNAL, SIG_HOLD) == handler);
  assert(set(TEST_SIGNAL, SIG_HOLD) == SIG_HOLD);
  sigset_t mask;
  assert(!sigprocmask(SIG_SETMASK, NULL, &mask));
  assert(sigismember(&mask, TEST_SIGNAL));
  assert(set(TEST_SIGNAL, handler) == SIG_HOLD);
  assert(!sigprocmask(SIG_SETMASK, NULL, &mask));
  assert(!sigismember(&mask, TEST_SIGNAL));
  observe(trace, action, routed);
  trace->registrations += 3;
#if defined(__GLIBC__) || defined(__ANDROID__)
  action_fn alias = lookup(library,
#ifdef __ANDROID__
                           "sigaction64"
#else
                           "__sigaction"
#endif
  );
  assert(!alias(TEST_SIGNAL, &reset, NULL));
  observe(trace, action, routed);
  ++trace->registrations;
#endif
  errno = 0;
  assert(bsd(0, handler) == SIG_ERR && errno == EINVAL);
  errno = 0;
  assert(interrupt(0, 1) == -1 && errno == EINVAL);
#ifdef __GLIBC__
  errno = 0;
  assert(bsd(TEST_SIGNAL, SIG_ERR) == SIG_ERR && errno == EINVAL);
#endif
  assert(!action(TEST_SIGNAL, &reset, NULL));
}

static void native_ignored_fault(action_fn native) {
  pid_t child = fork();
  assert(child >= 0);
  if (!child) {
    struct rlimit core = {0};
    assert(!setrlimit(RLIMIT_CORE, &core));
    struct sigaction ignored = {.sa_handler = SIG_IGN};
    sigemptyset(&ignored.sa_mask);
    sigset_t enabled;
    sigemptyset(&enabled);
    sigaddset(&enabled, SIGTRAP);
    assert(!native(SIGTRAP, &ignored, NULL));
    assert(!sigprocmask(SIG_UNBLOCK, &enabled, NULL));
#if defined(__x86_64__)
    __asm__ volatile("int3" ::: "memory");
#else
    __asm__ volatile("brk #0" ::: "memory");
#endif
    _exit(91);
  }
  int status;
  assert(waitpid(child, &status, 0) == child);
  assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGTRAP);
}

int main(void) {
#ifdef __ANDROID__
  void *libc = dlopen("libc.so", RTLD_NOW | RTLD_LOCAL);
#else
  void *libc = dlopen("libc.so.6", RTLD_NOW | RTLD_LOCAL);
#endif
  assert(libc);
  action_fn native = lookup(libc, "sigaction");
  struct sigaction original;
  assert(!native(TEST_SIGNAL, NULL, &original));
  sigset_t set, inherited;
  sigemptyset(&set);
  sigaddset(&set, TEST_SIGNAL);
  assert(!sigprocmask(SIG_UNBLOCK, &set, &inherited));
  struct trace baseline = {0}, routed = {0};
  /* Bionic helpers may call public sigaction64 through this executable too.
   * A disabled router forwards these native baseline calls unchanged. */
  assert(!signal_router_init_with_backend(native));
  assert(!signal_router_restore());
  exercise(libc, native, 0, &baseline);
  native_ignored_fault(native);
  assert(!native(TEST_SIGNAL, &original, NULL));
  assert(!signal_router_init_with_backend(native));
  exercise(RTLD_DEFAULT, signal_router_action, 1, &routed);
  assert(routed.count == baseline.count &&
         routed.registrations == baseline.registrations);
  for (unsigned i = 0; i < routed.count; ++i) {
    if (routed.actions[i].flags != baseline.actions[i].flags ||
        routed.actions[i].mask != baseline.actions[i].mask) {
      fprintf(stderr,
              "ALIAS_NATIVE_MISMATCH observation=%u "
              "flags=%x/%x mask=%llx/%llx\n",
              i, routed.actions[i].flags, baseline.actions[i].flags,
              (unsigned long long)routed.actions[i].mask,
              (unsigned long long)baseline.actions[i].mask);
    }
    assert(routed.actions[i].flags == baseline.actions[i].flags);
    assert(routed.actions[i].mask == baseline.actions[i].mask);
    assert(routed.actions[i].handler == baseline.actions[i].handler);
  }
  assert(!signal_router_restore());
  assert(!native(TEST_SIGNAL, &original, NULL));
  assert(!sigprocmask(SIG_SETMASK, &inherited, NULL));
  printf(
      "{\"result\":\"PASS\",\"registrations\":%u,"
      "\"native_comparisons\":%u,\"routed_deliveries\":%d,"
      "\"native_ignored_fault\":true}\n",
      routed.registrations, routed.count, deliveries / 2);
}
