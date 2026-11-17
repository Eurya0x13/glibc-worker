#define _GNU_SOURCE
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <threads.h>
#include <ucontext.h>
#include <unistd.h>

static int (*bind_mask)(uint64_t *, const uint64_t *, const int *, uint64_t);
static int (*initial_mask)(uint64_t *, uint64_t *);
static _Atomic unsigned started, finished;
static _Atomic int native_passthrough;
static const uint64_t owned = UINT64_C(1) << (SIGUSR1 - 1);

static _Thread_local struct {
  uint64_t logical, physical, initial_logical, initial_physical;
  uint64_t previous_observation, last_observation;
  unsigned observations;
  int active, bound, native;
} local;

static uint64_t physical_mask(void) {
  uint64_t mask;
  assert(!syscall(SYS_rt_sigprocmask, SIG_SETMASK, NULL, &mask, sizeof mask));
  return mask;
}

static void observe(uint64_t mask) {
  uint64_t blocked = UINT64_MAX & ~((UINT64_C(1) << (SIGKILL - 1)) |
                                    (UINT64_C(1) << (SIGSTOP - 1)));
  assert(physical_mask() == blocked && local.active && local.bound);
  assert(mask == local.logical);
  local.previous_observation = local.last_observation;
  local.last_observation = mask;
  ++local.observations;
  errno = ERANGE; /* The private adapter must preserve its caller's errno. */
}

static void lifecycle(int entering) {
  if (!entering) {
    assert(local.bound || local.native);
    atomic_fetch_add(&finished, 1);
    return;
  }
  assert(!initial_mask(&local.logical, &local.physical));
  local.initial_logical = local.logical;
  local.initial_physical = local.physical;
  if (atomic_load(&native_passthrough)) {
    local.native = 1;
    atomic_fetch_add(&started, 1);
    return;
  }
  /* Model a runtime-private wake bit which must stay physically unblocked
   * even if the inherited Guest logical mask blocks the same number. */
  local.physical &= ~owned;
  local.active = 1;
  assert(!bind_mask(&local.logical, &local.physical, &local.active, owned));
  local.bound = 1;
  atomic_fetch_add(&started, 1);
}

static int logical_usr1(void) {
  sigset_t mask;
  unsigned observations = local.observations;
  assert(!pthread_sigmask(SIG_SETMASK, NULL, &mask));
  assert(local.observations == observations);
  return sigismember(&mask, SIGUSR1);
}

static void change_usr1(int blocked) {
  sigset_t one;
  sigemptyset(&one);
  sigaddset(&one, SIGUSR1);
  errno = EDOM;
  assert(!sigprocmask(blocked ? SIG_BLOCK : SIG_UNBLOCK, &one, NULL));
  assert(errno == EDOM && logical_usr1() == blocked);
  assert(!(physical_mask() & owned));
}

static void *child(void *argument) {
  int expected = *(int *)argument;
  assert(local.bound && logical_usr1() == expected);
  assert(!(physical_mask() & owned));
  assert(!!(local.initial_logical & owned) == expected);
  assert(!!(local.initial_physical & owned) == expected);

  sigset_t one;
  sigemptyset(&one);
  sigaddset(&one, SIGUSR2);
  errno = EDOM;
  assert(!pthread_sigmask(SIG_BLOCK, &one, NULL) && errno == EDOM);
  assert(local.observations == 1 &&
         (local.last_observation & (UINT64_C(1) << (SIGUSR2 - 1))));
  change_usr1(!expected);

  uint64_t logical, physical;
  assert(!initial_mask(&logical, &physical));
  assert(logical == local.initial_logical &&
         physical == local.initial_physical);
  return argument;
}

static void pthread_case(int explicit_mask, int expected) {
  pthread_attr_t attributes;
  assert(!pthread_attr_init(&attributes));
  if (explicit_mask >= 0) {
    sigset_t mask;
    sigemptyset(&mask);
    if (explicit_mask) {
      sigaddset(&mask, SIGUSR1);
    }
    assert(!pthread_attr_setsigmask_np(&attributes, &mask));
  }
  int previous = logical_usr1();
  pthread_t thread;
  assert(!pthread_create(&thread, &attributes, child, &expected));
  assert(!pthread_attr_destroy(&attributes));
  void *result;
  assert(!pthread_join(thread, &result) && result == &expected);
  assert(logical_usr1() == previous);
}

static void *native_child(void *argument) {
  int expected = *(int *)argument;
  assert(local.native && !local.bound);
  assert(logical_usr1() == expected);
  assert(!!(physical_mask() & owned) == expected);
  assert(!local.observations);
  return argument;
}

static void native_case(void) {
  atomic_store(&native_passthrough, 1);
  int expected = 1;
  pthread_t thread;
  assert(!pthread_create(&thread, NULL, native_child, &expected));
  void *result;
  assert(!pthread_join(thread, &result) && result == &expected);
  atomic_store(&native_passthrough, 0);
}

static int c11_child(void *argument) {
  assert(child(argument) == argument);
  return 73;
}

static void context_body(void) {
  assert(logical_usr1() == 1 && !(physical_mask() & owned));
}

static void context_masks(void) {
  change_usr1(0);
  ucontext_t target;
  volatile sig_atomic_t resumed = 0;
  assert(!getcontext(&target));
  if (!resumed) {
    resumed = 1;
    change_usr1(1);
    assert(!setcontext(&target));
    assert(!"setcontext returned");
  }
  assert(!logical_usr1() && !(physical_mask() & owned));

  ucontext_t home, other;
  static _Alignas(16) unsigned char stack[65536];
  assert(!getcontext(&other));
  other.uc_stack = (stack_t){.ss_sp = stack, .ss_size = sizeof stack};
  other.uc_link = &home;
  sigaddset(&other.uc_sigmask, SIGUSR1);
  makecontext(&other, context_body, 0);
  assert(!swapcontext(&home, &other));
  assert(!logical_usr1() && !(physical_mask() & owned));
}

static void temporary_masks(void) {
  sigset_t temporary;
  sigemptyset(&temporary);
  sigaddset(&temporary, SIGUSR1);
  struct timespec zero = {0};
  unsigned before = local.observations;
  errno = EDOM;
  assert(!ppoll(NULL, 0, &zero, &temporary) && errno == EDOM);
  assert(local.observations == before + 2);
  assert(local.previous_observation & owned);
  assert(!(local.last_observation & owned));
  assert(!logical_usr1() && !(physical_mask() & owned));
  before = local.observations;
  assert(!ppoll(NULL, 0, &zero, NULL));
  assert(local.observations == before);
}

static void jump_masks(void) {
  change_usr1(0);
  sigjmp_buf saved;
  unsigned before = local.observations;
  if (!sigsetjmp(saved, 1)) {
    change_usr1(1);
    siglongjmp(saved, 1);
  }
  assert(!logical_usr1() && !(physical_mask() & owned));
  assert(local.observations == before + 2);

  sigjmp_buf unsaved;
  before = local.observations;
  if (!sigsetjmp(unsaved, 0)) {
    change_usr1(1);
    siglongjmp(unsaved, 1);
  }
  assert(logical_usr1() && !(physical_mask() & owned));
  assert(local.observations == before + 1);
  change_usr1(0);
}

int main(void) {
  *(void **)&bind_mask = dlsym(RTLD_DEFAULT, "__gw_domain_mask_bind");
  *(void **)&initial_mask = dlsym(RTLD_DEFAULT, "__gw_domain_mask_inherited");
  int (*mask_hook)(unsigned, void (*)(uint64_t));
  *(void **)&mask_hook = dlsym(RTLD_DEFAULT, "__gw_domain_mask_hook");
  int (*thread_hook)(void (*)(int));
  *(void **)&thread_hook = dlsym(RTLD_DEFAULT, "__gw_domain_thread_hook");
  assert(bind_mask && initial_mask && mask_hook && thread_hook);
  uint64_t sentinel_logical = 7193, sentinel_physical = 127;
  assert(initial_mask(&sentinel_logical, &sentinel_physical) == ENOTSUP);
  assert(sentinel_logical == 7193 && sentinel_physical == 127);

  uint64_t previous = physical_mask(), one = owned;
  assert(!syscall(SYS_rt_sigprocmask, SIG_UNBLOCK, &one, NULL, sizeof one));
  local.logical = local.physical = physical_mask();
  local.active = 1;
  assert(!bind_mask(&local.logical, &local.physical, &local.active, owned));
  local.bound = 1;
  assert(bind_mask(&local.logical, &local.physical, &local.active, owned) ==
         EALREADY);
  change_usr1(1); /* Optional observer has not been installed yet. */
  assert(!local.observations);
  assert(mask_hook(0, observe) == EINVAL && mask_hook(1, NULL) == EINVAL);
  assert(!mask_hook(1, observe));
  assert(mask_hook(1, observe) == EBUSY);
  assert(!thread_hook(lifecycle));

  native_case();
  pthread_case(-1, 1);
  pthread_case(0, 0);
  change_usr1(0);
  pthread_case(1, 1);
  change_usr1(1);
  thrd_t thread;
  int expected = 1, result;
  assert(thrd_create(&thread, c11_child, &expected) == thrd_success);
  assert(thrd_join(thread, &result) == thrd_success && result == 73);
  assert(atomic_load(&started) == 5 && atomic_load(&finished) == 5);
  context_masks();
  jump_masks();
  temporary_masks();

  local.active = 0;
  assert(!syscall(SYS_rt_sigprocmask, SIG_SETMASK, &previous, NULL,
                  sizeof previous));
  puts(
      "{\"result\":\"PASS\",\"threads\":5,\"logical_inheritance\":true,"
      "\"explicit_attributes\":true,\"c11_inheritance\":true,"
      "\"context_masks\":true,\"jump_masks\":true,"
      "\"temporary_masks\":true,\"errno\":true}");
}
