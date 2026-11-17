#define _GNU_SOURCE
#include "signal-router.h"
#include "signal-select.h"
#include "signal-user-copy.h"
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdatomic.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/* Native syscalls retain clock selection, timer saturation and restart rules.
 * Only explicit bootstrap integrations opt in to transparent notifications.
 * Remainders stay internal until a visible interruption actually needs user
 * copyout; success and absolute sleeps must not touch that output pointer.
 * https://github.com/torvalds/linux/blob/v6.12/kernel/time/hrtimer.c
 * https://github.com/bminor/glibc/blob/glibc-2.41/sysdeps/unix/sysv/linux/clock_nanosleep.c
 * https://github.com/aosp-mirror/platform_bionic/blob/main/libc/bionic/clock_nanosleep.cpp
 */
enum { S_CLOCK, S_NANO, S_MICRO, S_SECONDS, S_COUNT };

static _Atomic(void *) natives[S_COUNT];
static const char *names[S_COUNT] = {"clock_nanosleep", "nanosleep", "usleep",
                                     "sleep"};

static void *backend(unsigned index) {
  void *function = atomic_load_explicit(&natives[index], memory_order_acquire);
  if (!function) {
    function = dlsym(RTLD_NEXT, names[index]);
    if (function) {
      atomic_store_explicit(&natives[index], function, memory_order_release);
    }
  }
  return function;
}

int signal_sleep_prepare(void) {
  for (unsigned i = 0; i < S_COUNT; ++i) {
    if (!backend(i)) {
      return ENOSYS;
    }
  }
  return 0;
}

static __int128 nanoseconds(struct timespec time) {
  return (__int128)time.tv_sec * 1000000000 + time.tv_nsec;
}

static struct timespec duration(__int128 value) {
  const __int128 maximum = (__int128)INT64_MAX * 1000000000 + 999999999;
  value = value < 0 ? 0 : value > maximum ? maximum : value;
  return (struct timespec){value / 1000000000, value % 1000000000};
}

static int earlier(struct timespec first, struct timespec second) {
  return first.tv_sec < second.tv_sec ||
         (first.tv_sec == second.tv_sec && first.tv_nsec < second.tv_nsec);
}

struct sleep_budget {
  struct timespec requested, next, remainder;
  clockid_t clock;
  __int128 deadline;
  int absolute, timed;
};

static void begin_budget(struct sleep_budget *budget, clockid_t clock,
                         int flags, struct timespec requested) {
  *budget = (struct sleep_budget){
      .requested = requested,
      .next = requested,
      .clock = clock == CLOCK_REALTIME ? CLOCK_MONOTONIC : clock,
      .absolute = !!(flags & TIMER_ABSTIME)};
  struct timespec start;
  if (!budget->absolute && requested.tv_sec >= 0 && requested.tv_nsec >= 0 &&
      requested.tv_nsec < 1000000000 && !clock_gettime(budget->clock, &start)) {
    budget->timed = 1;
    budget->deadline = nanoseconds(start) + nanoseconds(requested);
  }
}

static int update_remainder(struct sleep_budget *budget, void *record) {
  if (!budget->timed) {
    return 0;
  }
  budget->deadline += gw_signal_native_wait_host_shift(record);

  struct timespec begin, end;
  int interval = gw_signal_native_wait_interval(record, &begin, &end);
  if (interval == ENOENT) {
    return 0; /* Native visible EINTR: keep the kernel's actual remainder. */
  }
  if (interval) {
    return interval;
  }

  struct timespec left = duration(budget->deadline - nanoseconds(end));
  if (gw_signal_native_wait_attempted(record)) {
    /* The kernel already applied its clock-specific timer saturation. Count
     * hidden work against that actual remainder too. A nested visible Host
     * handler ends this interval at entry, retaining native exclusion of its
     * own handler time. The original deadline independently prevents drift. */
    struct timespec native = duration(nanoseconds(budget->remainder) -
                                      (nanoseconds(end) - nanoseconds(begin)));
    if (earlier(native, left)) {
      left = native;
    }
  }
  budget->remainder = budget->next = left;
  return 0;
}

struct sleep_cleanup {
  void *record;
};

static void cancelled_sleep(struct sleep_cleanup *scope) {
  if (scope->record) {
    gw_signal_native_wait_cancel_end(scope->record);
    gw_signal_mask_wait_end(scope->record, -1, ECANCELED);
  }
}

static int native_sleep(clockid_t clock, int flags, struct timespec requested,
                        struct timespec *remaining) {
#ifndef __ANDROID__
  pthread_testcancel();
#endif
  void *record = gw_signal_mask_wait_current_begin();
  if (!record) {
    return errno;
  }
  /* Forced unwind invokes compiler cleanup metadata. No pthread cleanup
   * buffer points into this frame, so a native signal jump/context transfer
   * cannot leave a dangling libc cancellation chain. Router entry restores
   * cancellation type before exposing a user handler. */
  struct sleep_cleanup cleanup
      __attribute__((cleanup(cancelled_sleep))) = {.record = record};
  struct sleep_budget budget;
  begin_budget(&budget, clock, flags, requested);
  if (budget.timed) {
    gw_signal_native_wait_clock(record, budget.clock);
  }

  extern void gw_signal_mask_wait_test_point(int) __attribute__((weak));
  if (gw_signal_mask_wait_test_point) {
    gw_signal_mask_wait_test_point(1);
  }
#ifndef __ANDROID__
  pthread_testcancel();
#endif
  if (gw_signal_mask_wait_test_point) {
    gw_signal_mask_wait_test_point(2);
  }

  int result, error;
  for (;;) {
    gw_signal_native_wait_cancel_begin(record);
    budget.remainder = budget.next;
    result = gw_signal_wait_result(gw_signal_native_wait_syscall(
        record, SYS_clock_nanosleep, clock, flags, (long)&budget.next,
        budget.absolute ? 0 : (long)&budget.remainder, 0, 0));
    error = result < 0 ? errno : 0;
    if (error == EINTR) {
      int adjusted = update_remainder(&budget, record);
      if (adjusted) {
        error = adjusted;
        break;
      }
    }
    if (!gw_signal_native_wait_retry(record, result, error)) {
      break;
    }

    /* Absolute sleeps retry the same native deadline, including wall-clock
     * adjustments. Invalid clock/input calls still reach their native syscall.
     * Relative sleeps use the adjusted native remainder, never a ppoll value.
     */
    if (budget.timed) {
      budget.next = budget.remainder;
    }
    extern void gw_signal_mask_wait_retry_observe(long, const struct timespec *)
        __attribute__((weak));
    if (gw_signal_mask_wait_retry_observe) {
      gw_signal_mask_wait_retry_observe(SYS_clock_nanosleep, &budget.next);
    }
  }

  gw_signal_native_wait_cancel_end(record);
  if (error == EINTR && !budget.absolute && remaining) {
    int copied = gw_signal_user_copy_out(remaining, &budget.remainder,
                                         sizeof budget.remainder);
    if (copied) {
      error = copied;
    }
  }
  cleanup.record = NULL;
  gw_signal_mask_wait_end(record, error ? -1 : 0, error);
#ifndef __ANDROID__
  pthread_testcancel();
#endif
  return error;
}

int clock_nanosleep(clockid_t clock, int flags,
                    const struct timespec *requested,
                    struct timespec *remaining) {
  int saved = errno;
  typedef int (*function_type)(clockid_t, int, const struct timespec *,
                               struct timespec *);
  function_type native_clock = (function_type)backend(S_CLOCK);
  errno = saved;
  if (!native_clock) {
    errno = saved;
    return ENOSYS;
  }
  if (!signal_router_fault_notifications()) {
    return native_clock(clock, flags, requested, remaining);
  }
  if (clock == CLOCK_THREAD_CPUTIME_ID) {
    return EINVAL; /* Both carrier libcs reject it before the raw syscall. */
  }

  struct timespec copy;
  int error = gw_signal_user_copy_in(&copy, requested, sizeof copy);
  if (error == EFAULT) {
    /* Retain this libc's invalid input-pointer behavior, including a native
     * GNU dereference fault. This is not a potentially blocking valid call. */
    errno = saved;
    return native_clock(clock, flags, requested, remaining);
  }
  if (!error) {
#ifdef __GLIBC__
    /* glibc's PROCESS_CLOCK encodes pid zero and CPUCLOCK_SCHED as -6.
     * Keep this native frontend normalization without a signed left shift.
     * https://github.com/bminor/glibc/blob/glibc-2.41/sysdeps/unix/sysv/linux/kernel-posix-cpu-timers.h
     */
    if (clock == CLOCK_PROCESS_CPUTIME_ID) {
      clock = (clockid_t)-6;
    }
#endif
    error = native_sleep(clock, flags, copy, remaining);
  }
  errno = saved;
  return error;
}

int nanosleep(const struct timespec *requested, struct timespec *remaining) {
  int saved = errno;
  typedef int (*function_type)(const struct timespec *, struct timespec *);
  function_type native_nano = (function_type)backend(S_NANO);
  errno = saved;
  if (!native_nano) {
    errno = ENOSYS;
    return -1;
  }
  if (!signal_router_fault_notifications()) {
    return native_nano(requested, remaining);
  }
  int error = clock_nanosleep(CLOCK_REALTIME, 0, requested, remaining);
  if (error) {
    errno = error;
    return -1;
  }
  return 0;
}

int usleep(useconds_t microseconds) {
  int saved = errno;
  typedef int (*function_type)(useconds_t);
  function_type native_micro = (function_type)backend(S_MICRO);
  errno = saved;
  if (!native_micro) {
    errno = ENOSYS;
    return -1;
  }
  if (!signal_router_fault_notifications()) {
    return native_micro(microseconds);
  }
  struct timespec requested = {microseconds / 1000000,
                               (microseconds % 1000000) * 1000L};
  return nanosleep(&requested, NULL);
}

unsigned sleep(unsigned seconds) {
  int saved = errno;
  typedef unsigned (*function_type)(unsigned);
  function_type native_seconds = (function_type)backend(S_SECONDS);
  errno = saved;
  if (!native_seconds) {
    errno = ENOSYS;
    return seconds;
  }
  if (!signal_router_fault_notifications()) {
    return native_seconds(seconds);
  }
  struct timespec requested = {.tv_sec = seconds}, remaining = requested;
  return nanosleep(&requested, &remaining) ? (unsigned)remaining.tv_sec : 0;
}
