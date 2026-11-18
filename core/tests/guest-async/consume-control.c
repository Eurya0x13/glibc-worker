#define _GNU_SOURCE
#include "signal-actions.h"
#include "signal-consuming.h"
#include "signal-user-copy.h"
#include "nonlocal-mask.h"
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <sys/mman.h>

enum {
  WAIT_SIGNAL = 56,
  HELPER_SIGNAL = 53,
  GUEST_INTERRUPT = SIGUSR1,
  HOST_INTERRUPT = SIGUSR2
};

static struct gw_signal_targets targets;
static struct gw_signal_actions actions;
static __thread struct gw_signal_target *current;
static _Atomic int guest_interrupts, host_interrupts;

static int pending(sigset_t *set, int *handled) {
  *handled = current != NULL;
  return current ? gw_guest_sigpending(current, set) : 0;
}

static int consuming(const sigset_t *set, siginfo_t *information,
                     const struct timespec *timeout, gw_libc_wait_event event,
                     int *handled) {
  *handled = current != NULL;
  return current
             ? gw_guest_sigtimedwait(current, set, information, timeout, event)
             : 0;
}

static int notify(void *argument, struct gw_signal_target *target) {
  (void)argument;
  return syscall(SYS_tgkill, getpid(), target->tid, HELPER_SIGNAL) ? errno : 0;
}

static int send_signal(void *thread, int tid, const siginfo_t *information,
                       int *handled) {
  *handled = information->si_signo == WAIT_SIGNAL ||
             information->si_signo == GUEST_INTERRUPT;
  if (!*handled) {
    return 0;
  }
  struct gw_signal_publication result =
      gw_signal_actions_send(&actions, (uintptr_t)thread, tid, information);
  assert(!result.wake_error);
  return result.status;
}

static void guest_interrupt(int number) {
  assert(number == GUEST_INTERRUPT);
  atomic_fetch_add_explicit(&guest_interrupts, 1, memory_order_relaxed);
}

static void host_interrupt(int number) {
  assert(number == HOST_INTERRUPT);
  atomic_fetch_add_explicit(&host_interrupts, 1, memory_order_relaxed);
}

static void helper(int number) {
  int saved = errno;
  assert(number == HELPER_SIGNAL && current);
  struct gw_signal_delivery delivery;
  int received = gw_signal_actions_deliver(&actions, current, &delivery);
  assert(received == GUEST_INTERRUPT);
  assert(delivery.selected.action.sa_handler == guest_interrupt);
  gw_signal_target_note_delivery(
      current, delivery.selected.action.sa_flags & SA_RESTART);
  delivery.selected.action.sa_handler(received);
  errno = saved;
}

static sigset_t wait_set(void) {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, WAIT_SIGNAL);
  return set;
}

static int open_target(void) {
  int tid = (int)syscall(SYS_gettid);
  uint64_t mask = gw_nonlocal_mask_block();
  assert(!gw_signal_target_open(&targets, (uintptr_t)pthread_self(), tid,
                                UINT64_C(1) << (WAIT_SIGNAL - 1), NULL,
                                &current));
  gw_nonlocal_mask_restore(mask);
  return tid;
}

static void close_target(void *argument) {
  (void)argument;
  uint64_t mask = gw_nonlocal_mask_block();
  gw_signal_target_close(current);
  assert(!gw_signal_target_release(current));
  current = NULL;
  gw_nonlocal_mask_restore(mask);
}

static void check_queue_and_errors(void) {
  sigset_t set = wait_set(), reported;
  int tid = (int)syscall(SYS_gettid);
  assert(!syscall(SYS_tgkill, getpid(), tid, WAIT_SIGNAL));
  assert(!sigpending(&reported) && !sigismember(&reported, WAIT_SIGNAL));
  assert(!pthread_kill(pthread_self(), WAIT_SIGNAL));
  assert(!sigqueue(getpid(), WAIT_SIGNAL, (union sigval){.sival_int = 1104}));
  assert(!sigpending(&reported) && sigismember(&reported, WAIT_SIGNAL));

  siginfo_t info;
  struct timespec zero = {0};
  struct timespec invalid = {.tv_nsec = 1000000000};
  size_t page = (size_t)sysconf(_SC_PAGESIZE);
  void *bad = mmap(NULL, page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(bad != MAP_FAILED);
  assert(sigpending(bad) == -1 && errno == EFAULT);
  assert(sigtimedwait(bad, &info, &zero) == -1 && errno == EFAULT);
  assert(sigtimedwait(&set, &info, bad) == -1 && errno == EFAULT);
  assert(sigtimedwait(&set, &info, &invalid) == -1 && errno == EINVAL);
  errno = EDOM;
  assert(sigtimedwait(&set, &info, &zero) == WAIT_SIGNAL && errno == EDOM);
  assert(info.si_code == SI_USER && info.si_pid == getpid() &&
         info.si_uid == getuid());
  assert(sigwaitinfo(&set, &info) == WAIT_SIGNAL && info.si_code == SI_QUEUE &&
         info.si_value.sival_int == 1104);

  assert(!pthread_kill(pthread_self(), WAIT_SIGNAL));
  assert(sigtimedwait(&set, bad, &zero) == -1 && errno == EFAULT);
  assert(sigtimedwait(&set, bad, &zero) == -1 && errno == EAGAIN);
  assert(!pthread_kill(pthread_self(), WAIT_SIGNAL));
  int number = 0;
  assert(!sigwait(&set, &number) && number == WAIT_SIGNAL);
  assert(!sigpending(&reported) && !sigismember(&reported, WAIT_SIGNAL));
  assert(!munmap(bad, page));

  /* None of the public Guest waits may consume the original kernel packet. */
  uint64_t native_pending = 0;
  assert(!syscall(SYS_rt_sigpending, &native_pending, sizeof native_pending));
  assert(native_pending & (UINT64_C(1) << (WAIT_SIGNAL - 1)));
  assert(syscall(SYS_rt_sigtimedwait, &set, &info, &zero, sizeof(uint64_t)) ==
         WAIT_SIGNAL);
  assert(info.si_code == SI_TKILL);
}

struct wait_case {
  _Atomic int tid;
  int result, error, value;
};

static void *waiter(void *argument) {
  struct wait_case *test = argument;
  int tid = open_target();
  sigset_t set = wait_set();
  siginfo_t information;
  pthread_cleanup_push(close_target, NULL);
  atomic_store_explicit(&test->tid, tid, memory_order_release);
  errno = EDOM;
  test->result = sigwaitinfo(&set, &information);
  test->error = errno;
  if (test->result > 0) {
    test->value = information.si_value.sival_int;
  }
  pthread_cleanup_pop(1);
  return NULL;
}

static void wait_in_futex(struct wait_case *test) {
  struct timespec start, now;
  assert(!clock_gettime(CLOCK_MONOTONIC, &start));
  for (;;) {
    int tid = atomic_load_explicit(&test->tid, memory_order_acquire);
    if (tid) {
      char path[80];
      assert(snprintf(path, sizeof path, "/proc/self/task/%d/syscall", tid) >
             0);
      FILE *stream = fopen(path, "r");
      assert(stream);
      long syscall_number;
      unsigned long address;
      int fields = fscanf(stream, "%ld %lx", &syscall_number, &address);
      assert(!fclose(stream));
      if (fields == 2 && syscall_number == SYS_futex &&
          address == (uintptr_t)&targets.wait_sequence) {
        return;
      }
    }
    assert(!clock_gettime(CLOCK_MONOTONIC, &now));
    assert(now.tv_sec - start.tv_sec < 5);
    sched_yield();
  }
}

static void check_blocking(void) {
  pthread_t thread;
  struct wait_case test = {0};
  assert(!pthread_create(&thread, NULL, waiter, &test));
  wait_in_futex(&test);
  assert(
      !syscall(SYS_tgkill, getpid(), atomic_load(&test.tid), HOST_INTERRUPT));
  while (!atomic_load_explicit(&host_interrupts, memory_order_acquire)) {
    sched_yield();
  }
  wait_in_futex(&test);
  assert(!pthread_sigqueue(thread, WAIT_SIGNAL,
                           (union sigval){.sival_int = 59791}));
  assert(!pthread_join(thread, NULL));
  assert(test.result == WAIT_SIGNAL && test.error == EDOM &&
         test.value == 59791);

  atomic_store(&test.tid, 0);
  assert(!pthread_create(&thread, NULL, waiter, &test));
  wait_in_futex(&test);
  assert(!pthread_kill(thread, GUEST_INTERRUPT));
  assert(!pthread_join(thread, NULL));
  assert(test.result == -1 && test.error == EINTR &&
         atomic_load(&guest_interrupts) == 1);

  atomic_store(&test.tid, 0);
  assert(!pthread_create(&thread, NULL, waiter, &test));
  wait_in_futex(&test);
  assert(!pthread_cancel(thread));
  void *result = NULL;
  assert(!pthread_join(thread, &result) && result == PTHREAD_CANCELED);
}

static void configure(void) {
  assert(!gw_signal_user_copy_prepare());
  sigset_t set = wait_set();
  assert(!pthread_sigmask(SIG_BLOCK, &set, NULL));
  struct sigaction action = {.sa_handler = helper, .sa_flags = SA_RESTART};
  sigfillset(&action.sa_mask);
  assert(!sigaction(HELPER_SIGNAL, &action, NULL));
  action = (struct sigaction){.sa_handler = host_interrupt};
  sigemptyset(&action.sa_mask);
  assert(!sigaction(HOST_INTERRUPT, &action, NULL));

  uint64_t mask = gw_nonlocal_mask_block();
  gw_signal_targets_init(&targets, 0, 32, notify, NULL);
  assert(!gw_signal_targets_enable_waits(&targets));
  gw_signal_actions_init(&actions, &targets, 0);
  open_target();
  assert(!gw_signal_actions_representative(&actions, current));
  action.sa_handler = guest_interrupt;
  assert(!gw_signal_actions_change(&actions, GUEST_INTERRUPT, &action, NULL));
  gw_nonlocal_mask_restore(mask);

  int (*send_hook)(unsigned, int (*)(void *, int, const siginfo_t *, int *)) =
      dlsym(RTLD_DEFAULT, "__gw_domain_signal_send_hook");
  int (*consume_hook)(unsigned, const struct gw_libc_signal_consume_hook *) =
      dlsym(RTLD_DEFAULT, "__gw_domain_signal_consume_hook");
  assert(send_hook && consume_hook && !send_hook(1, send_signal));
  struct gw_libc_signal_consume_hook config = {
      .size = sizeof config, .pending = pending, .wait = consuming};
  assert(consume_hook(2, &config) == EINVAL);
  assert(!consume_hook(1, &config));
  assert(consume_hook(1, &config) == EBUSY);
}

int main(void) {
  configure();
  check_queue_and_errors();
  check_blocking();

  gw_nonlocal_mask_block();
  gw_signal_actions_close(&actions);
  close_target(NULL);
  gw_signal_targets_close(&targets);
  assert(!gw_signal_targets_destroy(&targets));
  puts(
      "{\"result\":\"PASS\",\"public_waits\":true,\"host_pending_separate\":"
      "true,"
      "\"timeout_and_copyout\":true,\"blocked_publication\":true,"
      "\"guest_eintr\":true,\"host_eintr_hidden\":true,\"native_cancel\":"
      "true}");
}
