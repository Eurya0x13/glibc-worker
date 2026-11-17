#define _GNU_SOURCE
#include "mask-inheritance.h"
#include <assert.h>
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <unistd.h>

static _Thread_local struct {
  uint64_t logical, physical;
  int active;
} binding;

static void *native_child(void *argument) {
  int expected = *(int *)argument;
  sigset_t observed;
  assert(!pthread_sigmask(SIG_SETMASK, NULL, &observed));
  assert(sigismember(&observed, SIGUSR1) == expected);
  uint64_t physical;
  assert(!syscall(SYS_rt_sigprocmask, SIG_SETMASK, NULL, &physical,
                  sizeof physical));
  assert(!!(physical & (UINT64_C(1) << (SIGUSR1 - 1))) == expected);
  return argument;
}

static void child_case(int explicit_mask, int expected) {
  pthread_attr_t attributes;
  assert(!pthread_attr_init(&attributes));
  if (explicit_mask) {
    sigset_t empty;
    sigemptyset(&empty);
    assert(!pthread_attr_setsigmask_np(&attributes, &empty));
  }
  pthread_t child;
  assert(!pthread_create(&child, &attributes, native_child, &expected));
  assert(!pthread_attr_destroy(&attributes));
  void *result;
  assert(!pthread_join(child, &result) && result == &expected);
}

void test_adopted_mask_inheritance(void) {
  int (*bind)(uint64_t *, const uint64_t *, const int *, uint64_t);
  *(void **)&bind = dlsym(RTLD_DEFAULT, "__gw_domain_mask_bind");
  assert(bind);
  uint64_t original, owned = UINT64_C(1) << (SIGUSR1 - 1);
  assert(!syscall(SYS_rt_sigprocmask, SIG_SETMASK, NULL, &original,
                  sizeof original));
  binding.logical = original | owned;
  binding.physical = original & ~owned;
  binding.active = 1;
  assert(!bind(&binding.logical, &binding.physical, &binding.active, owned));
  sigset_t one, observed;
  sigemptyset(&one);
  sigaddset(&one, SIGUSR1);
  assert(!pthread_sigmask(SIG_BLOCK, &one, NULL));
  assert(!pthread_sigmask(SIG_SETMASK, NULL, &observed));
  assert(sigismember(&observed, SIGUSR1));
  uint64_t actual;
  assert(
      !syscall(SYS_rt_sigprocmask, SIG_SETMASK, NULL, &actual, sizeof actual));
  assert(!(actual & owned));

  child_case(0, 1);
  child_case(1, 0);
  binding.active = 0;
  assert(!syscall(SYS_rt_sigprocmask, SIG_SETMASK, &original, NULL,
                  sizeof original));
}
