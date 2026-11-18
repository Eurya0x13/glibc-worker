#define _GNU_SOURCE
#include "signal-router.h"
#include "signal_channel.h"
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

enum { PROBE = 47, DELIVERY = SIGUSR1 };

static _Atomic int receiver, phase;
static _Atomic int nested, normal_deliveries;
static uintptr_t interrupted;

static uint64_t bit(int number) {
  return UINT64_C(1) << (number - 1);
}

static void publish(_Atomic int *word, int value) {
  atomic_store_explicit(word, value, memory_order_release);
  syscall(SYS_futex, word, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
}

static int await(_Atomic int *word, int value) {
  int observed;
  while ((observed = atomic_load_explicit(word, memory_order_acquire)) <
         value) {
    syscall(SYS_futex, word, FUTEX_WAIT_PRIVATE, observed, NULL, NULL, 0);
  }
  return observed;
}

static void receive(int number, siginfo_t *info, void *context) {
  (void)info;
  assert(number == DELIVERY);
  uint64_t effective;
  assert(!syscall(SYS_rt_sigprocmask, SIG_SETMASK, NULL, &effective, 8));
  assert((effective & (bit(SIGTERM) | bit(SIGALRM))) ==
         (bit(SIGTERM) | bit(SIGALRM)));
  assert(!(effective & (bit(PROBE) | bit(DELIVERY) | bit(SIGWINCH))));

  const ucontext_t *frame = context;
  assert(sigismember(&frame->uc_sigmask, DELIVERY));
  assert(sigismember(&frame->uc_sigmask, SIGWINCH));
  assert(!sigismember(&frame->uc_sigmask, SIGALRM));

  publish(&phase, 2);
  await(&phase, 3);
}

static void *recipient(void *unused) {
  (void)unused;
  uint64_t blocked = bit(DELIVERY) | bit(SIGWINCH);
  uint64_t temporary = bit(SIGALRM);
  assert(!syscall(SYS_rt_sigprocmask, SIG_SETMASK, &blocked, NULL, 8));
  publish(&receiver, (int)syscall(SYS_gettid));
  await(&phase, 1);

  struct timespec timeout = {.tv_sec = 5};
  assert(syscall(SYS_ppoll, NULL, 0, &timeout, &temporary, 8) == -1 &&
         errno == EINTR);
  uint64_t restored;
  assert(!syscall(SYS_rt_sigprocmask, SIG_SETMASK, NULL, &restored, 8));
  assert(restored == blocked);
  return NULL;
}

static void nested_handler(int number, siginfo_t *info, void *context) {
  (void)info;
  if (number != SIGURG) {
    atomic_fetch_add_explicit(&normal_deliveries, 1, memory_order_relaxed);
    return;
  }
  const ucontext_t *frame = context;
#if defined(__x86_64__)
  interrupted = frame->uc_mcontext.gregs[REG_RIP];
#else
  interrupted = frame->uc_mcontext.pc;
#endif
  publish(&nested, 1);
  await(&nested, 2);
}

static void *queue_nested(void *unused) {
  (void)unused;
  uint64_t all = bit(SIGUSR1) | bit(SIGUSR2) | bit(SIGURG);
  assert(!syscall(SYS_rt_sigprocmask, SIG_BLOCK, &all, NULL, 8));
  int tid = syscall(SYS_gettid);
  assert(!syscall(SYS_tgkill, getpid(), tid, SIGUSR1));
  assert(!syscall(SYS_tgkill, getpid(), tid, SIGUSR2));
  assert(!syscall(SYS_tgkill, getpid(), tid, SIGURG));
  assert(!syscall(SYS_rt_sigprocmask, SIG_UNBLOCK, &all, NULL, 8));
  return NULL;
}

static void check_nested(signal_router_backend native) {
  const int numbers[] = {SIGUSR1, SIGUSR2, SIGURG};
  struct sigaction action = {.sa_sigaction = nested_handler,
                             .sa_flags = SA_SIGINFO | SA_NODEFER};
  struct sigaction kernel[3];
  sigemptyset(&action.sa_mask);
  for (size_t i = 0; i < 3; ++i) {
    assert(!sigaction(numbers[i], &action, NULL));
    assert(!native(numbers[i], NULL, &kernel[i]));
  }

  pthread_t thread;
  assert(!pthread_create(&thread, NULL, queue_nested, NULL));
  await(&nested, 1);
  assert(interrupted == (uintptr_t)kernel[1].sa_sigaction);
  assert(!atomic_load_explicit(&normal_deliveries, memory_order_relaxed));

  action.sa_handler = SIG_IGN;
  action.sa_flags = 0;
  for (size_t i = 0; i < 3; ++i) {
    assert(!sigaction(numbers[i], &action, NULL));
  }
  unsigned released = 0;
  /* Both outer prefixes are still suspended. Only the deepest, bound
   * callback and an earlier data-only action can leave the allocation list. */
  assert(!gw_host_signal_reclaim(1000, &released) && released == 2);
  unsigned char resident;
  for (size_t i = 0; i < 2; ++i) {
    assert(!mincore((void *)kernel[i].sa_sigaction, 1, &resident));
  }
  assert(mincore((void *)kernel[2].sa_sigaction, 1, &resident) == -1 &&
         errno == ENOMEM);

  publish(&nested, 2);
  assert(!pthread_join(thread, NULL));
  assert(atomic_load_explicit(&normal_deliveries, memory_order_relaxed) == 2);
}

int main(void) {
  signal_router_backend native = dlsym(RTLD_NEXT, "sigaction");
  assert(native && !signal_router_init() && !signal_router_adopt());

  struct sigaction action = {.sa_sigaction = receive,
                             .sa_flags = SA_SIGINFO | SA_NODEFER};
  sigemptyset(&action.sa_mask);
  sigaddset(&action.sa_mask, SIGTERM);
  assert(!sigaction(DELIVERY, &action, NULL));
  struct sigaction published, logical;
  assert(!native(DELIVERY, NULL, &published));
  assert(!sigaction(DELIVERY, NULL, &logical));
  assert(logical.sa_sigaction == receive && (logical.sa_flags & SA_NODEFER));
  assert(sigismember(&logical.sa_mask, SIGTERM));
  assert(!sigismember(&logical.sa_mask, PROBE));

  /* The generation predates the lease. Kernel-mask matching still uses
   * the exact temporary public mask supplied to the raw ppoll syscall. */
  assert(!gw_host_signal_reclaim_enable(PROBE));
  pthread_t thread;
  assert(!pthread_create(&thread, NULL, recipient, NULL));
  assert(!syscall(SYS_tgkill, getpid(), await(&receiver, 1), DELIVERY));
  publish(&phase, 1);
  await(&phase, 2);

  struct sigaction ignored = {.sa_handler = SIG_IGN};
  sigemptyset(&ignored.sa_mask);
  assert(!sigaction(DELIVERY, &ignored, &logical));
  assert(logical.sa_sigaction == receive);
  assert(!signal_router_verify());
  unsigned released = 0;
  assert(!gw_host_signal_reclaim(1000, &released) && released == 1);
  unsigned char resident;
  assert(mincore((void *)published.sa_sigaction, 1, &resident) == -1 &&
         errno == ENOMEM);

  publish(&phase, 3);
  assert(!pthread_join(thread, NULL));
  check_nested(native);
  assert(!signal_router_restore());
  puts(
      "{\"result\":\"PASS\",\"ordinary_mask\":true,\"pre_lease\":true,"
      "\"nested_prefixes\":true}");
}
