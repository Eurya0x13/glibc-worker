#define _GNU_SOURCE
#include "signal-targets.h"
#include "thread-create.h"
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

enum mode { ORDINARY, PARENT_FIRST, CHILD_FIRST, CANCELLED, REJECT_PREPARE };

struct creation {
  struct gw_signal_target *target;
  void *key;
  uint64_t logical, physical;
  enum mode mode;
  _Atomic unsigned holds;
  _Atomic int left;
};

static struct gw_signal_targets registry;
static enum mode mode;
static int child_only;
static _Atomic int gate, child_waiting, cancelled_cleanup;
static _Atomic unsigned preparations, completions, entries, exits, wakes;
static unsigned setup_failures, clone_failures, early_exits;
static _Thread_local struct creation *current;
static _Thread_local int lifecycle;
static _Thread_local int destroyed;
static _Thread_local int prepare_errno, completion_errno;
static pthread_key_t destructor_key;
static int descriptors[2];
static int (*inherited)(uint64_t *, uint64_t *);

static void critical_mask(void) {
  uint64_t actual;
  assert(
      !syscall(SYS_rt_sigprocmask, SIG_SETMASK, NULL, &actual, sizeof actual));
  uint64_t catchable = UINT64_MAX & ~(UINT64_C(1) << (SIGKILL - 1)) &
                       ~(UINT64_C(1) << (SIGSTOP - 1));
  assert(actual == catchable);
}

static void await(_Atomic int *value) {
  struct timespec start, now;
  assert(!clock_gettime(CLOCK_MONOTONIC, &start));
  while (!atomic_load_explicit(value, memory_order_acquire)) {
    assert(!clock_gettime(CLOCK_MONOTONIC, &now));
    assert(now.tv_sec - start.tv_sec < 3);
    sched_yield();
  }
}

static void release(struct creation *creation) {
  assert(!gw_signal_target_unpin(creation->target));
  if (atomic_fetch_sub_explicit(&creation->holds, 1, memory_order_acq_rel) ==
      1) {
    assert(!gw_signal_target_release(creation->target));
    assert(!munmap(creation, sizeof *creation));
  }
}

static int notify(void *argument, struct gw_signal_target *target) {
  assert(argument == &registry && target->tid > 0);
  atomic_fetch_add(&wakes, 1);
  return 0;
}

static int prepare(void *key, uint64_t logical, uint64_t physical,
                   void **cookie) {
  prepare_errno = errno;
  critical_mask();
  atomic_fetch_add(&preparations, 1);
  errno = ERANGE;
  if (mode == REJECT_PREPARE) {
    return ENOMEM;
  }

  struct creation *creation =
      mmap(NULL, sizeof *creation, PROT_READ | PROT_WRITE,
           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(creation != MAP_FAILED);
  creation->key = key;
  creation->logical = logical;
  creation->physical = physical;
  creation->mode = mode;
  atomic_init(&creation->holds, 2);
  assert(!gw_signal_target_prepare(&registry, (uintptr_t)key, logical, creation,
                                   &creation->target));
  assert(!gw_signal_target_retain(creation->target));
  *cookie = creation;
  return 0;
}

static void complete(void *cookie, int tid, int error) {
  completion_errno = errno;
  critical_mask();
  struct creation *creation = cookie;
  atomic_fetch_add(&completions, 1);
  if (error) {
    if (tid > 0) {
      ++setup_failures;
    } else {
      ++clone_failures;
    }
    assert(!atomic_load(&creation->left));
    gw_signal_target_close(creation->target);
    release(creation); /* The failed child never enters the lifecycle. */
  } else {
    assert(tid > 0);
    if (creation->mode == CHILD_FIRST) {
      /* Deliberately delay the parent across detached child exit. The
       * callback must only use its cookie, never the possibly unmapped PD. */
      await(&creation->left);
      assert(gw_signal_target_activate(creation->target, tid) == ESRCH);
      ++early_exits;
    } else if (!(child_only && creation->mode == PARENT_FIRST)) {
      int activation = gw_signal_target_activate(creation->target, tid);
      assert(!activation || activation == ESRCH);
      if (creation->mode == PARENT_FIRST) {
        assert(!activation);
      }
    }
  }
  errno = ERANGE;
  release(creation);
}

static void enter(void *cookie) {
  critical_mask();
  struct creation *creation = cookie;
  assert(lifecycle == 1 && !current);
  assert((void *)pthread_self() == creation->key);
  uint64_t logical, physical;
  assert(!inherited(&logical, &physical));
  assert(logical == creation->logical && physical == creation->physical);
  current = creation;
  if (creation->mode == PARENT_FIRST) {
    /* Native startup is held before our admission. Parent completion must
     * still make public thread sends work before pthread_create returns. */
    atomic_store_explicit(&child_waiting, 1, memory_order_release);
    await(&gate);
  }
  assert(!gw_signal_target_activate(creation->target, syscall(SYS_gettid)));
  atomic_fetch_add(&entries, 1);
  errno = ERANGE;
}

static void leave(void *cookie) {
  critical_mask();
  struct creation *creation = cookie;
  assert(lifecycle == 1 && current == creation && destroyed);
  gw_signal_target_close(creation->target);
  lifecycle = 2;
  current = NULL;
  atomic_fetch_add(&exits, 1);
  atomic_store_explicit(&creation->left, 1, memory_order_release);
  errno = ERANGE;
  release(creation);
}

static void legacy_lifecycle(int entering) {
  if (entering) {
    assert(!lifecycle);
    lifecycle = 1;
  } else {
    assert(lifecycle == 2);
    lifecycle = 0;
  }
}

static int send_signal(void *key, int tid, const siginfo_t *information,
                       int *handled) {
  *handled = information->si_signo == 56;
  if (!*handled) {
    return 0;
  }
  struct gw_signal_publication result =
      gw_signal_targets_send(&registry, (uintptr_t)key, tid, information);
  assert(!result.wake_error);
  return result.status;
}

static void cleanup(void *argument) {
  assert(current && argument == &cancelled_cleanup);
  atomic_store_explicit(&cancelled_cleanup, 1, memory_order_release);
}

static void destructor(void *argument) {
  assert(current && argument == &registry && !destroyed);
  destroyed = 1;
}

static void *user(void *argument) {
  assert(current && lifecycle == 1 && errno != ERANGE);
  assert(!pthread_setspecific(destructor_key, &registry));
  if (current->mode == CANCELLED) {
    pthread_cleanup_push(cleanup, &cancelled_cleanup);
    atomic_store_explicit(&child_waiting, 1, memory_order_release);
    char byte;
    read(descriptors[0], &byte, 1);
    assert(0);
    pthread_cleanup_pop(0);
  }
  if (current->mode == PARENT_FIRST && !child_only) {
    uint64_t all = UINT64_MAX;
    assert(!syscall(SYS_rt_sigprocmask, SIG_SETMASK, &all, NULL, sizeof all));
    siginfo_t information;
    enum gw_signal_queue_source source;
    assert(gw_signal_target_take(current->target, UINT64_MAX, &information,
                                 &source) == 56);
    assert(source == GW_SIGNAL_THREAD && information.si_code == SI_QUEUE &&
           information.si_value.sival_int == 731);
  }
  if (argument == &preparations) {
    pthread_exit(argument);
  }
  return argument;
}

static void prestart_case(void) {
  pthread_t thread;
  void *result;
  mode = PARENT_FIRST;
  uint64_t signal_bit = UINT64_C(1) << 55;
  assert(!syscall(SYS_rt_sigprocmask, SIG_UNBLOCK, &signal_bit, NULL,
                  sizeof signal_bit));
  assert(!pthread_create(&thread, NULL, user, &registry));
  await(&child_waiting);
  int sent = pthread_sigqueue(thread, 56, (union sigval){.sival_int = 731});
  assert(sent == (child_only ? ESRCH : 0));
  assert(!!atomic_load(&wakes) == !child_only);
  atomic_store_explicit(&gate, 1, memory_order_release);
  assert(!pthread_join(thread, &result) && result == &registry);
  assert(!syscall(SYS_rt_sigprocmask, SIG_BLOCK, &signal_bit, NULL,
                  sizeof signal_bit));
}

static void ordinary_cases(void) {
  pthread_t thread;
  void *result;
  for (unsigned i = 0; i < 64; ++i) {
    mode = ORDINARY;
    errno = EDOM;
    void *argument = i % 2 ? (void *)&preparations : (void *)&registry;
    assert(!pthread_create(&thread, NULL, user, argument));
    /* Native clone3 fallback can leave ENOSYS even after successful create.
     * Preserve its actual value, not an invented whole-API errno promise. */
    assert(errno == completion_errno);
    assert(!pthread_join(thread, &result) && result == argument);
  }

  prestart_case();

  pthread_attr_t attributes;
  assert(!pthread_attr_init(&attributes));
  assert(!pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED));
  mode = CHILD_FIRST;
  for (unsigned i = 0; i < 64; ++i) {
    assert(!pthread_create(&thread, &attributes, user, NULL));
  }
  assert(!pthread_attr_destroy(&attributes));

  mode = CANCELLED;
  atomic_store(&child_waiting, 0);
  assert(!pipe(descriptors));
  assert(!pthread_create(&thread, NULL, user, NULL));
  await(&child_waiting);
  assert(!pthread_cancel(thread));
  assert(!pthread_join(thread, &result) && result == PTHREAD_CANCELED);
  assert(atomic_load_explicit(&cancelled_cleanup, memory_order_acquire));
  assert(!close(descriptors[0]) && !close(descriptors[1]));
}

static void failure_cases(void) {
  pthread_t thread;
  unsigned completed = atomic_load(&completions);
  unsigned entered = atomic_load(&entries);
  mode = REJECT_PREPARE;
  errno = EDOM;
  assert(pthread_create(&thread, NULL, user, NULL) == EAGAIN);
  assert(errno == prepare_errno && atomic_load(&completions) == completed);

  mode = ORDINARY;
  pthread_attr_t attributes;
  cpu_set_t empty;
  CPU_ZERO(&empty);
  assert(!pthread_attr_init(&attributes));
  assert(!pthread_attr_setaffinity_np(&attributes, sizeof empty, &empty));
  assert(pthread_create(&thread, &attributes, user, NULL) == EINVAL);
  assert(!pthread_attr_destroy(&attributes));
  assert(setup_failures == 1 && atomic_load(&entries) == entered);

  /* Final case: force actual clone failure on this control's parent only.
   * No TSYNC and no more threads are needed after installing this filter. */
  struct sock_filter instructions[] = {
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_clone, 1, 0),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_clone3, 0, 1),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EAGAIN),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)};
  struct sock_fprog filter = {
      .len = sizeof instructions / sizeof instructions[0],
      .filter = instructions};
  assert(!prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0));
  assert(!prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &filter));
  assert(pthread_create(&thread, NULL, user, NULL) == EAGAIN);
  assert(clone_failures == 1 && atomic_load(&entries) == entered);
}

int main(int argc, char **argv) {
  assert(argc == 1 || (argc == 2 && !strcmp(argv[1], "--child-only")));
  child_only = argc == 2;
  int (*install)(unsigned, const struct gw_libc_thread_create_hook *);
  int (*legacy)(void (*)(int));
  int (*send_hook)(unsigned, int (*)(void *, int, const siginfo_t *, int *));
  *(void **)&install = dlsym(RTLD_DEFAULT, "__gw_domain_thread_create_hook");
  *(void **)&legacy = dlsym(RTLD_DEFAULT, "__gw_domain_thread_hook");
  *(void **)&send_hook = dlsym(RTLD_DEFAULT, "__gw_domain_signal_send_hook");
  *(void **)&inherited = dlsym(RTLD_DEFAULT, "__gw_domain_mask_inherited");
  assert(install && legacy && send_hook && inherited);

  uint64_t all = UINT64_MAX;
  assert(!syscall(SYS_rt_sigprocmask, SIG_SETMASK, &all, NULL, sizeof all));
  gw_signal_targets_init(&registry, 0, 128, notify, &registry);
  assert(!pthread_key_create(&destructor_key, destructor));
  struct gw_libc_thread_create_hook hook = {.size = sizeof hook,
                                            .prepare = prepare,
                                            .complete = complete,
                                            .enter = enter,
                                            .leave = leave};
  assert(install(0, &hook) == EINVAL);
  assert(install(1, NULL) == EINVAL);
  --hook.size;
  assert(install(1, &hook) == EINVAL);
  ++hook.size;
  assert(!install(1, &hook));
  assert(install(1, &hook) == EBUSY);
  hook = (struct gw_libc_thread_create_hook){0}; /* Libc copied the config. */
  assert(!legacy(legacy_lifecycle));
  assert(!send_hook(1, send_signal));

  if (child_only) {
    prestart_case();
    assert(!registry.count && !registry.retained);
    gw_signal_targets_close(&registry);
    assert(!gw_signal_targets_destroy(&registry));
    puts("{\"result\":\"PASS\",\"missing_parent_admission_detected\":true}");
    return 0;
  }

  ordinary_cases();
  failure_cases();
  assert(atomic_load(&entries) == 130 && atomic_load(&exits) == 130);
  assert(atomic_load(&preparations) == 133);
  assert(atomic_load(&completions) == 132 && early_exits == 64);
  assert(!registry.count && !registry.retained);
  gw_signal_targets_close(&registry);
  assert(!gw_signal_targets_destroy(&registry));
  puts(
      "{\"result\":\"PASS\",\"children\":130,\"early_exits\":64,"
      "\"prestart_send\":true,\"cancel_cleanup\":true,"
      "\"prepare_failure\":true,\"clone_failure\":true,"
      "\"setup_failure\":true}");
}
