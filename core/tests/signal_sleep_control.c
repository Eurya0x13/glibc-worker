#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/* Native-only design control: ppoll is not a drop-in nanosleep replacement.
 * Use the actual kernel to check remainder copyout and timer saturation.
 * No glibc-worker library or signal interposer is linked to this executable.
 * https://github.com/torvalds/linux/blob/v6.12/kernel/time/hrtimer.c
 * See do_nanosleep(), nanosleep_copyout() and __hrtimer_start_range_ns().
 * https://github.com/torvalds/linux/blob/v6.12/fs/select.c
 * See poll_select_set_timeout() and poll_select_finish(). */
static volatile sig_atomic_t delivered;

struct interrupt {
  pthread_t target;
  _Atomic int done;
};

static void receive(int number) {
  assert(number == SIGUSR1);
  delivered = 1;
}

static void *interrupt_wait(void *argument) {
  struct interrupt *state = argument;
  while (!atomic_load_explicit(&state->done, memory_order_acquire)) {
    int result = pthread_kill(state->target, SIGUSR1);
    assert(!result);
    /* Repeat until the target returns: the first send can precede syscall
     * entry. No fixed sleep assumes that the target has already blocked. */
    assert(!syscall(SYS_sched_yield));
  }
  return NULL;
}

enum operation { SLEEP_LIBC, SLEEP_CLOCK, POLL_KERNEL };

static int interrupted_call(enum operation operation,
                            const struct timespec *request,
                            struct timespec *remaining) {
  struct interrupt state = {.target = pthread_self()};
  pthread_t sender;
  assert(!pthread_create(&sender, NULL, interrupt_wait, &state));
  delivered = 0;
  errno = EDOM;

  int result;
  if (operation == SLEEP_LIBC) {
    result = nanosleep(request, remaining);
  } else if (operation == SLEEP_CLOCK) {
    result = clock_nanosleep(CLOCK_MONOTONIC, 0, request, remaining);
  } else {
    *remaining = *request;
    result = syscall(SYS_ppoll, NULL, 0, remaining, NULL, sizeof(uint64_t));
  }

  int error = errno;
  atomic_store_explicit(&state.done, 1, memory_order_release);
  assert(!pthread_join(sender, NULL));
  assert(delivered);
  errno = error;
  return result;
}

static void remainder_copyout(void *inaccessible) {
  const struct timespec zero = {0};
  errno = EDOM;
  assert(!nanosleep(&zero, inaccessible) && errno == EDOM);
  assert(!clock_nanosleep(CLOCK_MONOTONIC, 0, &zero, inaccessible) &&
         errno == EDOM);

  const struct timespec duration = {.tv_sec = 60};
  assert(interrupted_call(SLEEP_LIBC, &duration, inaccessible) == -1 &&
         errno == EFAULT);
  assert(interrupted_call(SLEEP_CLOCK, &duration, inaccessible) == EFAULT &&
         errno == EDOM);
}

static void timer_saturation(void) {
  const struct timespec enormous = {.tv_sec = INT64_MAX, .tv_nsec = 999999999};
  struct timespec remaining;
  assert(interrupted_call(SLEEP_LIBC, &enormous, &remaining) == -1 &&
         errno == EINTR);
  assert(remaining.tv_sec > 0 && remaining.tv_sec <= INT64_MAX / 1000000000);
  assert(remaining.tv_nsec >= 0 && remaining.tv_nsec < 1000000000);

  assert(interrupted_call(SLEEP_CLOCK, &enormous, &remaining) == EINTR &&
         errno == EDOM);
  assert(remaining.tv_sec > 0 && remaining.tv_sec <= INT64_MAX / 1000000000);
  assert(remaining.tv_nsec >= 0 && remaining.tv_nsec < 1000000000);

  assert(interrupted_call(POLL_KERNEL, &enormous, &remaining) == -1 &&
         errno == EINTR);
  assert(remaining.tv_sec > INT64_MAX / 1000000000);
  assert(remaining.tv_nsec >= 0 && remaining.tv_nsec < 1000000000);
}

int main(void) {
  struct sigaction action = {.sa_handler = receive, .sa_flags = SA_RESTART};
  struct sigaction previous;
  sigemptyset(&action.sa_mask);
  assert(!sigaction(SIGUSR1, &action, &previous));
  sigset_t set, inherited;
  sigemptyset(&set);
  sigaddset(&set, SIGUSR1);
  assert(!pthread_sigmask(SIG_UNBLOCK, &set, &inherited));

  long page = sysconf(_SC_PAGESIZE);
  assert(page > 0);
  void *inaccessible =
      mmap(NULL, page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(inaccessible != MAP_FAILED);
  remainder_copyout(inaccessible);
  timer_saturation();

  assert(!munmap(inaccessible, page));
  assert(!pthread_sigmask(SIG_SETMASK, &inherited, NULL));
  assert(!sigaction(SIGUSR1, &previous, NULL));
  puts(
      "{\"result\":\"CONTROL_ONLY\",\"native_only\":true,"
      "\"sa_restart\":true,\"success_ignores_remainder\":true,"
      "\"interrupted_remainder_efault\":true,\"clock_errno_preserved\":true,"
      "\"different_timeout_saturation\":true}");
}
