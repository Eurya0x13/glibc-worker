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
#ifdef GW_ROUTED_SLEEP
#include "signals.h"
#include "signal-router.h"
#include "nonlocal-mask.h"
#include <dlfcn.h>
#endif

/* Native-only design control and matching opt-in sleep regression. The control
 * links no worker or interposer; GW_ROUTED_SLEEP uses the same kernel contract
 * with bootstrap routing and additional hidden-notification exercises.
 * https://github.com/torvalds/linux/blob/v6.12/kernel/time/hrtimer.c
 * See do_nanosleep(), nanosleep_copyout() and __hrtimer_start_range_ns().
 * https://github.com/torvalds/linux/blob/v6.12/fs/select.c
 * See poll_select_set_timeout() and poll_select_finish(). */
static volatile sig_atomic_t delivered;

#ifdef GW_ROUTED_SLEEP
static int inject_hidden, hidden_mode, delayed, nested, retried, absolute_test;

void gw_signal_mask_wait_test_point(int stage) {
  if (stage == 2 && inject_hidden) {
    inject_hidden = 0;
    assert(!syscall(SYS_tgkill, getpid(), syscall(SYS_gettid),
                    signal_router_fault_notifications()[0]));
  }
}

void signal_router_pending_observe(int number) {
  if (hidden_mode == 3 && !delayed &&
      number == signal_router_fault_notifications()[0]) {
    delayed = 1;
    struct timespec work = {.tv_nsec = 150000000};
    assert(!syscall(SYS_clock_nanosleep, CLOCK_MONOTONIC, 0, &work, NULL));
  }
}

void gw_signal_hidden_wait_test_point(void) {
  if (hidden_mode == 4) {
    uint64_t bit = UINT64_C(1) << (SIGUSR1 - 1);
    assert(!syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), SIGUSR1));
    assert(!syscall(SYS_rt_sigprocmask, SIG_UNBLOCK, &bit, NULL, sizeof bit));
    gw_nonlocal_mask_block();
  } else if (hidden_mode == 5 && !nested) {
    nested = 1;
    struct timespec zero = {0};
    assert(!nanosleep(&zero, NULL));
  }
}

void gw_signal_mask_wait_retry_observe(long number,
                                       const struct timespec *remaining) {
  assert(number == SYS_clock_nanosleep);
  ++retried;
  if (hidden_mode == 3 && !absolute_test) {
    assert(delayed && !remaining->tv_sec && !remaining->tv_nsec);
  }
}

static void hidden_sleeps(void) {
  for (int api = 0; api < 3; ++api) {
    for (int scenario = 1; scenario <= 5; ++scenario) {
      if (scenario == 2) {
        continue;
      }
      hidden_mode = scenario;
      inject_hidden = 1;
      delayed = nested = retried = 0;
      delivered = 0;
      struct timespec request = {.tv_nsec = 100000000}, saved = request;
      struct timespec remaining = {.tv_sec = 7193, .tv_nsec = 17};
      errno = EDOM;
      int result =
          api == 0   ? nanosleep(&request, &remaining)
          : api == 1 ? clock_nanosleep(CLOCK_MONOTONIC, 0, &request, &remaining)
                     : usleep(100000);
      if (scenario == 4) {
        assert(delivered);
        assert(result == (api == 1 ? EINTR : -1));
        assert(errno == (api == 1 ? EDOM : EINTR));
        if (api < 2) {
          assert(remaining.tv_sec == 0 && remaining.tv_nsec >= 0 &&
                 remaining.tv_nsec <= request.tv_nsec);
        }
        assert(!retried);
      } else {
        assert(!result && errno == EDOM && !delivered);
        assert(retried == 1);
        assert(remaining.tv_sec == 7193 && remaining.tv_nsec == 17);
      }
      assert(request.tv_sec == saved.tv_sec &&
             request.tv_nsec == saved.tv_nsec);
      assert(!inject_hidden && delayed == (scenario == 3));
      assert(nested == (scenario == 5));
      hidden_mode = 0;
    }
  }

  hidden_mode = 4;
  inject_hidden = 1;
  delivered = retried = 0;
  assert(sleep(1) == 0 && errno == EINTR && delivered && !inject_hidden);
  hidden_mode = 0;
}

static void clock_variants(void *inaccessible) {
  typedef int (*function_type)(clockid_t, int, const struct timespec *,
                               struct timespec *);
  function_type native = (function_type)dlsym(RTLD_NEXT, "clock_nanosleep");
  assert(native);
  const clockid_t clocks[] = {CLOCK_REALTIME,
                              CLOCK_MONOTONIC,
                              CLOCK_BOOTTIME,
                              CLOCK_TAI,
                              CLOCK_PROCESS_CPUTIME_ID,
                              CLOCK_THREAD_CPUTIME_ID,
                              -1};
  struct timespec zero = {0};
  for (unsigned i = 0; i < sizeof clocks / sizeof clocks[0]; ++i) {
    errno = EDOM;
    int expected = native(clocks[i], 0, &zero, inaccessible);
    assert(errno == EDOM);
    int observed = clock_nanosleep(clocks[i], 0, &zero, inaccessible);
    if (observed != expected) {
      fprintf(stderr, "CLOCK_VARIANT clock=%d expected=%d observed=%d\n",
              (int)clocks[i], expected, observed);
    }
    assert(observed == expected);
    assert(errno == EDOM);
  }

  for (unsigned i = 0; i < 4; ++i) {
    struct timespec until;
    if (clock_gettime(clocks[i], &until)) {
      continue;
    }
    until.tv_nsec += 1000000;
    if (until.tv_nsec >= 1000000000) {
      ++until.tv_sec;
      until.tv_nsec -= 1000000000;
    }
    hidden_mode = 3;
    absolute_test = 1;
    inject_hidden = 1;
    delayed = 0;
    errno = EDOM;
    assert(!clock_nanosleep(clocks[i], TIMER_ABSTIME, &until, inaccessible));
    assert(errno == EDOM && !inject_hidden && delayed);
    hidden_mode = 0;
    absolute_test = 0;
  }
}
#endif

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
#ifdef GW_ROUTED_SLEEP
  assert(!signal_router_init());
  int notifications[GW_SIGNAL_FAULT_LANES];
  for (int i = 0; i < GW_SIGNAL_FAULT_LANES; ++i) {
    notifications[i] = SIGRTMIN + i;
  }
  assert(!signal_router_defer_faults(notifications));
  struct exchange exchange = {.host_tid = syscall(SYS_gettid)};
  struct signal_slot *recipient = signal_acquire(&exchange);
  assert(recipient);
#endif
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
#ifdef GW_ROUTED_SLEEP
  hidden_sleeps();
  clock_variants(inaccessible);
#endif

  assert(!munmap(inaccessible, page));
  assert(!pthread_sigmask(SIG_SETMASK, &inherited, NULL));
  assert(!sigaction(SIGUSR1, &previous, NULL));
#ifdef GW_ROUTED_SLEEP
  assert(!signal_prepare_release(recipient, 0));
  signal_release(recipient);
  assert(!signal_router_restore());
  puts(
      "{\"result\":\"PASS\",\"native_sleep_contract\":true,"
      "\"sa_restart\":true,\"success_ignores_remainder\":true,"
      "\"interrupted_remainder_efault\":true,\"clock_errno_preserved\":true,"
      "\"different_timeout_saturation\":true,\"hidden_sleep_notifications\":"
      "true,"
      "\"native_sleep_clocks\":true}");
#else
  puts(
      "{\"result\":\"CONTROL_ONLY\",\"native_only\":true,"
      "\"sa_restart\":true,\"success_ignores_remainder\":true,"
      "\"interrupted_remainder_efault\":true,\"clock_errno_preserved\":true,"
      "\"different_timeout_saturation\":true}");
#endif
}
