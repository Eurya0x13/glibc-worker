#define _GNU_SOURCE
#include "signal-router.h"
#include "signal_channel.h"
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <jni.h>
#include <linux/futex.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

static int proof_number, guarded_number, ordinary_number;
static signal_router_backend native_action;
static struct sigaction vm_fault;
static _Atomic int receiver, phase, entered, returned, delivered, stopped;

static void advance(_Atomic int *word, int value) {
  atomic_store_explicit(word, value, memory_order_release);
  syscall(SYS_futex, word, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
}

static void await(_Atomic int *word, int value) {
  int observed;
  while ((observed = atomic_load_explicit(word, memory_order_acquire)) <
         value) {
    syscall(SYS_futex, word, FUTEX_WAIT_PRIVATE, observed, NULL, NULL, 0);
  }
}

static void old_handler(int number) {
  assert(number == guarded_number);
  advance(&entered, 1);
  await(&phase, 1);

  /* The logical action initially blocks the proof channel. Keep the old
   * callback frame live after unblocking it so the collector has to rely
   * on copied metadata through the rest of the signal return. */
  uint64_t proof_bit = UINT64_C(1) << (proof_number - 1);
  assert(!syscall(SYS_rt_sigprocmask, SIG_UNBLOCK, &proof_bit, NULL, 8));
  advance(&entered, 2);
  await(&phase, 2);
  advance(&returned, 1);
}

static void new_handler(int number) {
  assert(number == guarded_number);
  advance(&delivered, 1);
}

static void ordinary_handler(int number) {
  assert(number == ordinary_number);
}

static void install(int number, void (*handler)(int), int guarded) {
  struct sigaction action = {.sa_handler = handler, .sa_flags = SA_RESTART};
  sigemptyset(&action.sa_mask);
  if (guarded) {
    /* GNU sigfillset omits its two private NPTL signals. */
    const uint64_t all = UINT64_MAX;
    memcpy(&action.sa_mask, &all, sizeof all);
  }
  assert(!signal_router_action(number, &action, NULL));
}

static void *kernel_entry(int number) {
  struct sigaction action;
  assert(!native_action(number, NULL, &action));
  return (void *)action.sa_sigaction;
}

static int mapped(void *entry) {
  unsigned char pages;
  if (!mincore(entry, 1, &pages)) {
    return 1;
  }
  assert(errno == ENOMEM);
  return 0;
}

JNIEXPORT void JNICALL Java_compat_SignalReclaimSuite_prepare(JNIEnv *env,
                                                              jclass type) {
  (void)env;
  (void)type;
#ifdef __ANDROID__
  const char *name = "libc.so";
#else
  const char *name = "libc.so.6";
#endif
  void *libc = dlopen(name, RTLD_NOW | RTLD_LOCAL);
  assert(libc);
  native_action = dlsym(libc, "sigaction");
  assert(native_action && !native_action(SIGSEGV, NULL, &vm_fault));
  assert(vm_fault.sa_handler != SIG_DFL && vm_fault.sa_handler != SIG_IGN);
  assert(!signal_router_init_with_backend(native_action));

  proof_number = 47;
  guarded_number = 48;
  ordinary_number = 49;
  assert(gw_host_signal_reclaim_enable(63) == EBUSY);
  int status = gw_host_signal_reclaim_enable(proof_number);
  if (status) {
    struct sigaction kernel;
    uint64_t mask;
    assert(!native_action(proof_number, NULL, &kernel));
    memcpy(&mask, &kernel.sa_mask, sizeof mask);
    fprintf(stderr,
            "Lease RT%d: %s (%d), native handler=%p flags=%#x mask=%#llx\n",
            proof_number, strerror(status), status, (void *)kernel.sa_sigaction,
            kernel.sa_flags, (unsigned long long)mask);
  }
  assert(!status);
  assert(gw_host_signal_reclaim_enable(proof_number) == EALREADY);
  assert(signal_router_action(proof_number, NULL, NULL) == -1 &&
         errno == EBUSY);
  install(guarded_number, old_handler, 1);
  install(ordinary_number, ordinary_handler, 0);
}

JNIEXPORT void JNICALL Java_compat_SignalReclaimSuite_receive(JNIEnv *env,
                                                              jclass type) {
  (void)env;
  (void)type;
  advance(&receiver, (int)syscall(SYS_gettid));
  await(&stopped, 1);
}

JNIEXPORT void JNICALL Java_compat_SignalReclaimSuite_reclaim(JNIEnv *env,
                                                              jclass type) {
  (void)env;
  (void)type;
  await(&receiver, 1);
  int tid = atomic_load_explicit(&receiver, memory_order_relaxed);
  void *guarded = kernel_entry(guarded_number);
  void *ordinary = kernel_entry(ordinary_number);
  assert(mapped(guarded) && mapped(ordinary));
  assert(!syscall(SYS_tgkill, getpid(), tid, guarded_number));
  await(&entered, 1);

  install(guarded_number, new_handler, 1);
  install(ordinary_number, ordinary_handler, 0);
  unsigned reclaimed = 23;
  unsigned before = signal_router_retained();
  assert(gw_host_signal_reclaim(50, &reclaimed) == EBUSY);
  assert(reclaimed == 23 && signal_router_retained() == before);
  assert(mapped(guarded) && mapped(ordinary));

  advance(&phase, 1);
  await(&entered, 2);
  int status = gw_host_signal_reclaim(1000, &reclaimed);
  if (status) {
    fprintf(stderr, "Live native rendezvous: %s (%d)\n", strerror(status),
            status);
  }
  assert(!status);
  assert(reclaimed == 1 && signal_router_retained() == before - 1);
  assert(!mapped(guarded) && mapped(ordinary));
  assert(!atomic_load_explicit(&returned, memory_order_acquire));
  assert(mapped(kernel_entry(guarded_number)));

  advance(&phase, 2);
  await(&returned, 1);
  assert(!syscall(SYS_tgkill, getpid(), tid, guarded_number));
  await(&delivered, 1);
  advance(&stopped, 1);

  struct sigaction actual;
  assert(!native_action(SIGSEGV, NULL, &actual));
  assert(actual.sa_sigaction == vm_fault.sa_sigaction &&
         actual.sa_flags == vm_fault.sa_flags);
  assert(!signal_router_verify());
}
