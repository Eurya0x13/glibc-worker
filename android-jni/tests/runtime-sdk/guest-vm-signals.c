#define _GNU_SOURCE
#include "vm-signals.h"
#include <jni.h>
#include <assert.h>
#include <errno.h>
#include <fenv.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

enum { STACK_BYTES = 128 * 1024 };

struct probe_stack {
  ucontext_t saved, entry;
  void *mapping;
  char *base;
};

struct vm_probe {
  struct probe_stack outer, inner;
  struct sdk_vm_signals *control;
  JNIEnv *env;
  jclass observer;
  jmethodID callback;
  jobject shared;
  pthread_t recipient;
  uintptr_t tp;
  long tid;
};

static __thread struct vm_probe *current_probe;

static void check_frame(const struct vm_probe *probe, int rounding,
                        int masked) {
  sigset_t mask;
  assert(fegetround() == rounding && errno == EDOM);
  assert(!pthread_sigmask(SIG_SETMASK, NULL, &mask));
  assert(sigismember(&mask, SIGURG) == masked);
  assert(pthread_equal(pthread_self(), probe->recipient));
  assert((uintptr_t)__builtin_thread_pointer() == probe->tp);
  assert(syscall(SYS_gettid) == probe->tid);
}

static void wait_for_command(const struct vm_probe *probe, int command) {
  while (atomic_load_explicit(&probe->control->command, memory_order_acquire) <
         command) {
    sched_yield();
  }
}

static void sample_on_inner_stack(void) {
  struct vm_probe *probe = current_probe;
  volatile unsigned canary = 0x9f7530u;
  assert((uintptr_t)&canary - (uintptr_t)probe->inner.base < STACK_BYTES);
  errno = EDOM;
  check_frame(probe, FE_UPWARD, 0);

  atomic_store_explicit(&probe->control->stage, SDK_VM_NATIVE_READY,
                        memory_order_release);
  wait_for_command(probe, SDK_VM_JAVA_READY);

  JNIEnv *env = probe->env;
  (*env)->CallStaticVoidMethod(env, probe->observer, probe->callback,
                               probe->shared);
  assert(!(*env)->ExceptionCheck(env));
  (*env)->DeleteLocalRef(env, probe->observer);
  check_frame(probe, FE_UPWARD, 0);

  atomic_store_explicit(&probe->control->stage, SDK_VM_JAVA_RETURNED,
                        memory_order_release);
  wait_for_command(probe, SDK_VM_STACKS_RELEASED);

  assert(canary == 0x9f7530u);
  check_frame(probe, FE_UPWARD, 0);
}

static void park_on_outer_stack(void) {
  struct vm_probe *probe = current_probe;
  volatile unsigned canary = 0xa57e31u;
  assert((uintptr_t)&canary - (uintptr_t)probe->outer.base < STACK_BYTES);
  assert(!fesetround(FE_DOWNWARD));
  errno = EDOM;
  check_frame(probe, FE_DOWNWARD, 1);
  assert(!swapcontext(&probe->inner.saved, &probe->inner.entry));
  assert(canary == 0xa57e31u);
  check_frame(probe, FE_DOWNWARD, 1);
}

static void prepare_stack(struct probe_stack *stack, size_t page,
                          void (*entry)(void)) {
  stack->mapping = mmap(NULL, STACK_BYTES + 2 * page, PROT_NONE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(stack->mapping != MAP_FAILED);
  stack->base = (char *)stack->mapping + page;
  assert(!mprotect(stack->base, STACK_BYTES, PROT_READ | PROT_WRITE));
  assert(!getcontext(&stack->entry));
  stack->entry.uc_stack =
      (stack_t){.ss_sp = stack->base, .ss_size = STACK_BYTES};
  stack->entry.uc_link = &stack->saved;
  makecontext(&stack->entry, entry, 0);
}

jint sdk_guest_vm_signals(JNIEnv *env, jclass cls, jlong address,
                          jobject shared) {
  (void)cls;
  jclass observer = (*env)->FindClass(env, "SdkVmSignals");
  assert(observer);
  jmethodID callback = (*env)->GetStaticMethodID(
      env, observer, "sampleWhileGuestParked", "(Ljava/nio/ByteBuffer;)V");
  assert(callback);
  struct vm_probe probe = {.control = (void *)(uintptr_t)address,
                           .env = env,
                           .observer = observer,
                           .callback = callback,
                           .shared = shared,
                           .recipient = pthread_self(),
                           .tp = (uintptr_t)__builtin_thread_pointer(),
                           .tid = syscall(SYS_gettid)};
  assert(!current_probe);
  current_probe = &probe;

  int rounding = fegetround();
  size_t page = (size_t)sysconf(_SC_PAGESIZE);
  assert(page <= STACK_BYTES && !(STACK_BYTES % page));
  sigset_t set, previous;
  sigemptyset(&set);
  sigaddset(&set, SIGURG);
  assert(!pthread_sigmask(SIG_BLOCK, &set, &previous));
  prepare_stack(&probe.outer, page, park_on_outer_stack);
  assert(!pthread_sigmask(SIG_UNBLOCK, &set, NULL));
  assert(!fesetround(FE_UPWARD));
  prepare_stack(&probe.inner, page, sample_on_inner_stack);
  assert(!fesetround(FE_TOWARDZERO));

  volatile unsigned canary = 0xb58302u;
  errno = EDOM;
  assert(!swapcontext(&probe.outer.saved, &probe.outer.entry));
  assert(canary == 0xb58302u);
  check_frame(&probe, FE_TOWARDZERO, 0);

  assert(!munmap(probe.inner.mapping, STACK_BYTES + 2 * page));
  assert(!munmap(probe.outer.mapping, STACK_BYTES + 2 * page));
  assert(!pthread_sigmask(SIG_SETMASK, &previous, NULL));
  assert(!fesetround(rounding));
  current_probe = NULL;
  atomic_store_explicit(&probe.control->stage, SDK_VM_STACKS_RELEASED,
                        memory_order_release);
  return 73;
}
