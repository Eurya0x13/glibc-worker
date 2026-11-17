#define _GNU_SOURCE
#include "signals.h"
#include "signal-router.h"
#include "signal-select.h"
#include "nonlocal-mask.h"
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <time.h>
#include <ucontext.h>

static struct signal_slot *recipient;
static volatile sig_atomic_t deliveries, jumps, jump_mode, frame_edit;
static sigjmp_buf checkpoint;
static sigset_t blocked, temporary;
static int epoll_fd;
static uint64_t private_bits;
static int hidden_mode, inject_hidden, retry_seen, delayed, plain_deliveries;
static int nested_hidden;
static int consume_hidden;
static int ready_pipe[2];

static uint64_t bits(const sigset_t *mask) {
  uint64_t result;
  memcpy(&result, mask, sizeof result);
  return result;
}

static void receive(int number, siginfo_t *information, void *context) {
  int saved_errno = errno;
  assert(number == SIGTRAP || number == SIGUSR1);
  ucontext_t *frame = context;
  assert(!(bits(&frame->uc_sigmask) & private_bits));
  sigset_t mask;
  assert(!pthread_sigmask(SIG_SETMASK, NULL, &mask));
  assert(!(bits(&mask) & private_bits));
  if (number == SIGTRAP) {
    assert(information->si_code == SI_QUEUE);
    assert(information->si_pid == getpid());
    assert(information->si_uid == getuid());
    assert(information->si_value.sival_int == 7193);
    /* Kernel mask-waits save the pre-wait (blocked) mask in ucontext while
     * delivering under their temporary mask. Native sigreturn restores it. */
    assert(sigismember(&frame->uc_sigmask, SIGTRAP));
    assert(!sigismember(&mask, SIGTRAP)); /* SA_NODEFER */
    ++deliveries;
    if (jump_mode) {
      ++jumps;
      siglongjmp(checkpoint, 1);
    }
  } else {
    ++plain_deliveries;
    if (!frame_edit) {
      errno = saved_errno;
      return;
    }
    if (frame_edit == 1) {
      sigdelset(&frame->uc_sigmask, SIGTRAP);
    } else {
      sigaddset(&frame->uc_sigmask, SIGTRAP);
    }
  }
  errno = saved_errno;
}

void gw_signal_mask_wait_test_point(int stage) {
  if (stage == 2 && inject_hidden) {
    inject_hidden = 0;
    int number = signal_router_fault_notifications()[0];
    assert(!syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), number));
  }
}

void gw_signal_selection_sleep_test_point(void) {
  if (consume_hidden) {
    consume_hidden = 0;
    int number = signal_router_fault_notifications()[0];
    assert(!syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), number));
  }
}

void signal_router_pending_observe(int number) {
  if (hidden_mode == 3 && !delayed &&
      number == signal_router_fault_notifications()[0]) {
    delayed = 1;
    struct timespec work = {.tv_nsec = 150000000};
    /* Simulate time spent in hidden work; the kernel has already copied its
     * remainder before entering this notification frame. */
    assert(!syscall(SYS_nanosleep, &work, NULL));
  }
}

void gw_signal_mask_wait_retry_observe(long number,
                                       const struct timespec *remaining) {
  (void)number;
  if (!hidden_mode) {
    return;
  }
  ++retry_seen;
  if (hidden_mode == 3) {
    assert(delayed && remaining && !remaining->tv_sec && !remaining->tv_nsec);
  }
  if (hidden_mode == 2) {
    assert(!syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), SIGUSR1));
  } else if (hidden_mode == 6) {
    assert(write(ready_pipe[1], "x", 1) == 1);
  }
}

void gw_signal_hidden_wait_test_point(void) {
  if (hidden_mode == 4) {
    uint64_t bit = UINT64_C(1) << (SIGUSR1 - 1);
    assert(!syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), SIGUSR1));
    assert(!syscall(SYS_rt_sigprocmask, SIG_UNBLOCK, &bit, NULL, sizeof bit));
    gw_nonlocal_mask_block();
  } else if (hidden_mode == 5 && !nested_hidden) {
    nested_hidden = 1;
    struct timespec zero = {0};
    assert(!ppoll(NULL, 0, &zero, &temporary));
  }
}

static void queue(void) {
  uint64_t previous = gw_nonlocal_mask_block();
  siginfo_t information = {.si_signo = SIGTRAP, .si_code = SI_QUEUE};
  information.si_pid = getpid();
  information.si_uid = getuid();
  information.si_value.sival_int = 7193;
  if (recipient) {
    assert(!gw_signal_deferred_put(recipient->deferred, &information));
  } else {
    assert(!syscall(SYS_rt_tgsigqueueinfo, getpid(), syscall(SYS_gettid),
                    SIGTRAP, &information));
  }
  gw_nonlocal_mask_restore(previous);
}

static void check_pending(int expected) {
  sigset_t pending;
  assert(!sigpending(&pending));
  assert(sigismember(&pending, SIGTRAP) == expected);
  assert(!(bits(&pending) & private_bits));
}

static void check_mask(int trap_blocked) {
  sigset_t observed;
  assert(!pthread_sigmask(SIG_SETMASK, NULL, &observed));
  assert(sigismember(&observed, SIGTRAP) == trap_blocked);
  assert(!(bits(&observed) & private_bits));
}

static void consume(void) {
  sigset_t wanted;
  sigemptyset(&wanted);
  sigaddset(&wanted, SIGTRAP);
  struct timespec zero = {0};
  siginfo_t information;
  assert(sigtimedwait(&wanted, &information, &zero) == SIGTRAP);
  assert(information.si_value.sival_int == 7193);
  check_pending(0);
}

static int wait_call(int mode, const sigset_t *mask,
                     const struct timespec *timeout) {
  struct epoll_event event;
  switch (mode) {
    case 100:
      return ppoll(NULL, 0, timeout, NULL);
    case 101:
      return pselect(0, NULL, NULL, NULL, timeout, NULL);
    case 102:
      return epoll_pwait(epoll_fd, &event, 1, 100, NULL);
    case 103:
      return wait_call(4, NULL, timeout);
    case 104:
      return poll(NULL, 0, 100);
    case 105:
      return epoll_wait(epoll_fd, &event, 1, 100);
    case 106: {
      struct timeval duration = {.tv_usec = 100000};
      int result = select(0, NULL, NULL, NULL, &duration);
      assert(!duration.tv_sec && duration.tv_usec >= 0 &&
             duration.tv_usec <= 100000);
      if (!result) {
        assert(!duration.tv_usec);
      }
      return result;
    }
    case 0:
      return sigsuspend(mask);
    case 1:
      return ppoll(NULL, 0, timeout, mask);
    case 2:
      return pselect(0, NULL, NULL, NULL, timeout, mask);
    case 3:
      return epoll_pwait(epoll_fd, &event, 1,
                         timeout->tv_sec || timeout->tv_nsec ? 100 : 0, mask);
    case 4: {
      typedef int (*function_type)(int, struct epoll_event *, int,
                                   const struct timespec *, const sigset_t *);
      function_type function =
          (function_type)dlsym(RTLD_DEFAULT, "epoll_pwait2");
      assert(function);
      return function(epoll_fd, &event, 1, timeout, mask);
    }
#ifdef __ANDROID__
    case 5:
      return sigsuspend64((const sigset64_t *)mask);
    case 6:
      return ppoll64(NULL, 0, timeout, (const sigset64_t *)mask);
    case 7:
      return pselect64(0, NULL, NULL, NULL, timeout, (const sigset64_t *)mask);
    case 8:
      return epoll_pwait64(epoll_fd, &event, 1,
                           timeout->tv_sec || timeout->tv_nsec ? 100 : 0,
                           (const sigset64_t *)mask);
    case 9: {
      typedef int (*function_type)(int, struct epoll_event *, int,
                                   const struct timespec *, const sigset64_t *);
      function_type function =
          (function_type)dlsym(RTLD_DEFAULT, "epoll_pwait2_64");
      assert(function);
      return function(epoll_fd, &event, 1, timeout, (const sigset64_t *)mask);
    }
#endif
    default:
      assert(0);
  }
  return -1;
}

static unsigned exercise(int mode) {
  int before = deliveries;
  queue();
  check_pending(1);
  struct timespec timeout = {.tv_nsec = 100000000}, saved = timeout;
  int result = wait_call(mode, &temporary, &timeout);
  assert(!memcmp(&timeout, &saved, sizeof timeout));
  if (result == -1 && errno == ENOSYS) {
    assert(mode == 4 || mode == 9);
    assert(deliveries == before);
    check_pending(1);
    consume();
    return 0;
  }
  assert(result == -1 && errno == EINTR);
  assert(deliveries == before + 1);
  check_pending(0);
  check_mask(1);
  if (mode % 5) {
    /* A zero-time blocked or NULL temporary mask must neither deliver nor
     * consume a queued original. The immutable timeout is retained too. */
    timeout = (struct timespec){0};
    queue();
    assert(!wait_call(mode, &blocked, &timeout));
    assert(!wait_call(mode, NULL, &timeout));
    check_pending(1);
    assert(deliveries == before + 1);
    consume();
    check_mask(1);
  }
  return 1;
}

static void wait_jumps(void) {
  for (int save = 0; save <= 1; ++save) {
    assert(!pthread_sigmask(SIG_SETMASK, &blocked, NULL));
    queue();
    jump_mode = 1;
    if (!sigsetjmp(checkpoint, save)) {
      sigsuspend(&temporary);
      assert(0);
    }
    jump_mode = 0;
    check_mask(save);
    check_pending(0);
  }
  assert(jumps == 2);
  assert(!pthread_sigmask(SIG_SETMASK, &blocked, NULL));
}

static void transparent_waits(int count) {
  for (int variant = 0; variant < count + 7; ++variant) {
    int mode = variant < count ? variant : 100 + variant - count;
    for (int scenario = 1; scenario <= 5; ++scenario) {
      if ((mode == 0 || mode == 5) && scenario != 2 && scenario != 4) {
        continue; /* sigsuspend requires a real Host handler to complete. */
      }
      hidden_mode = scenario;
      inject_hidden = 1;
      retry_seen = delayed = nested_hidden = 0;
      int before = deliveries, host_before = plain_deliveries;
      struct timespec timeout = {.tv_nsec = 100000000}, saved = timeout;
      errno = EDOM;
      int result = wait_call(mode, &temporary, &timeout);
      if (result == -1 && errno == ENOSYS) {
        assert(mode == 4 || mode == 9 || mode == 103);
        inject_hidden = 0;
        hidden_mode = 0;
        continue;
      }

      if (scenario == 2 || scenario == 4) {
        assert(result == -1 && errno == EINTR);
        assert(plain_deliveries == host_before + 1);
        assert(retry_seen == (scenario == 2));
      } else {
        assert(!result && errno == EDOM && retry_seen == 1);
        assert(plain_deliveries == host_before);
      }
      assert(deliveries == before && !memcmp(&timeout, &saved, sizeof timeout));
      assert(!inject_hidden && nested_hidden == (scenario == 5));
      assert(delayed == (scenario == 3));
      hidden_mode = 0;
      check_mask(1);
    }
  }

  consume_hidden = 1;
  sigset_t wanted;
  sigemptyset(&wanted);
  sigaddset(&wanted, SIGALRM);
  struct timespec timeout = {.tv_nsec = 1000000};
  siginfo_t info;
  assert(sigtimedwait(&wanted, &info, &timeout) == -1 && errno == EAGAIN);
  assert(!consume_hidden);

  for (int boundary = 0; boundary < 3; ++boundary) {
    hidden_mode = 2;
    inject_hidden = 1;
    retry_seen = 0;
    int before = plain_deliveries;
    struct timespec enormous = {.tv_sec = INT64_MAX, .tv_nsec = 999999999};
    int result = boundary == 0   ? ppoll(NULL, 0, &enormous, &temporary)
                 : boundary == 1 ? ppoll(NULL, 0, NULL, NULL)
                                 : poll(NULL, 0, -7);
    assert(result == -1 && errno == EINTR && retry_seen == 1);
    assert(plain_deliveries == before + 1);
    assert(enormous.tv_sec == INT64_MAX && enormous.tv_nsec == 999999999);
    hidden_mode = 0;
  }

  struct timespec invalid = {.tv_nsec = 1000000000};
  assert(ppoll(NULL, 0, &invalid, &temporary) == -1 && errno == EINVAL);
}

static int ready_wait(int mode, struct timespec *timeout) {
  struct pollfd poll_fd = {.fd = ready_pipe[0], .events = POLLIN};
  struct epoll_event event;
  fd_set readers;
  FD_ZERO(&readers);
  FD_SET(ready_pipe[0], &readers);
  int result;
  switch (mode) {
    case 0:
      result = ppoll(&poll_fd, 1, timeout, &temporary);
      break;
    case 1:
      result = poll(&poll_fd, 1, 1000);
      break;
    case 2:
      result =
          pselect(ready_pipe[0] + 1, &readers, NULL, NULL, timeout, &temporary);
      break;
    case 3: {
      struct timeval duration = {.tv_sec = 1};
      result = select(ready_pipe[0] + 1, &readers, NULL, NULL, &duration);
      assert(!duration.tv_sec && duration.tv_usec > 0);
      break;
    }
    case 4:
      result = epoll_pwait(epoll_fd, &event, 1, 1000, &temporary);
      break;
    case 5:
      result = epoll_wait(epoll_fd, &event, 1, 1000);
      break;
    default:
      assert(0);
      return -1;
  }

  assert(result == 1);
  if (mode < 2) {
    assert(poll_fd.revents == POLLIN);
  } else if (mode < 4) {
    assert(FD_ISSET(ready_pipe[0], &readers));
  } else {
    assert(event.events == EPOLLIN && event.data.u64 == 7519);
  }
  return result;
}

static void readiness_after_retry(void) {
  assert(!pipe(ready_pipe) && ready_pipe[0] < FD_SETSIZE);
  struct epoll_event event = {.events = EPOLLIN, .data.u64 = 7519};
  assert(!epoll_ctl(epoll_fd, EPOLL_CTL_ADD, ready_pipe[0], &event));

  for (int mode = 0; mode < 6; ++mode) {
    hidden_mode = 6;
    inject_hidden = 1;
    retry_seen = 0;
    struct timespec timeout = {.tv_sec = 1}, saved = timeout;
    errno = EDOM;
    assert(ready_wait(mode, &timeout) == 1 && errno == EDOM);
    assert(!inject_hidden && retry_seen == 1);
    assert(!memcmp(&timeout, &saved, sizeof timeout));
    char byte;
    assert(read(ready_pipe[0], &byte, 1) == 1 && byte == 'x');
    hidden_mode = 0;
    check_mask(1);
  }

  assert(!epoll_ctl(epoll_fd, EPOLL_CTL_DEL, ready_pipe[0], NULL));
  assert(!close(ready_pipe[0]) && !close(ready_pipe[1]));
}

static void select_native_comparison(void) {
  typedef int (*function_type)(int, fd_set *, fd_set *, fd_set *,
                               struct timeval *);
  function_type native = (function_type)dlsym(RTLD_NEXT, "select");
  assert(native);
  const struct timeval inputs[] = {{0, 0},
                                   {0, -1},
                                   {-1, 0},
                                   {0, 1000000},
                                   {INT64_MAX, 1000000},
                                   {0, INT64_C(1) << 32}};
  for (unsigned i = 0; i < sizeof inputs / sizeof inputs[0]; ++i) {
    struct timeval expected = inputs[i], observed = expected;
    struct timespec start, finish;
    assert(!clock_gettime(CLOCK_MONOTONIC, &start));
    errno = 0;
    int baseline = native(-1, NULL, NULL, NULL, &expected), error = errno;
    errno = 0;
    assert(select(-1, NULL, NULL, NULL, &observed) == baseline &&
           errno == error);
    assert(!clock_gettime(CLOCK_MONOTONIC, &finish));

    /* Both calls may decrease the remaining timeout, including on EINVAL.
     * Their difference is bounded by the measured enclosing interval rather
     * than an arbitrary timing tolerance or exact sub-microsecond equality. */
    struct timeval larger = expected, smaller = observed;
    if (expected.tv_sec < observed.tv_sec ||
        (expected.tv_sec == observed.tv_sec &&
         expected.tv_usec < observed.tv_usec)) {
      larger = observed;
      smaller = expected;
    }
    time_t seconds = larger.tv_sec - smaller.tv_sec;
    long microseconds = larger.tv_usec - smaller.tv_usec;
    if (microseconds < 0) {
      --seconds;
      microseconds += 1000000;
    }
    finish.tv_sec -= start.tv_sec;
    finish.tv_nsec -= start.tv_nsec;
    if (finish.tv_nsec < 0) {
      --finish.tv_sec;
      finish.tv_nsec += 1000000000;
    }
    assert(seconds < finish.tv_sec ||
           (seconds == finish.tv_sec &&
            microseconds <= (finish.tv_nsec + 999) / 1000));
  }
}

static long gateway_sleep(void *record, const struct timespec *duration) {
  return gw_signal_native_wait_syscall(
      record, SYS_clock_nanosleep, CLOCK_MONOTONIC, 0, (long)duration, 0, 0, 0);
}

static void native_wait_gateway(void) {
  struct timespec short_wait = {.tv_nsec = 1000000};
  void *record = gw_signal_mask_wait_current_begin();
  assert(record);
  gw_signal_mask_wait_cancel_check(record);
  assert(!gateway_sleep(record, &short_wait));
  assert(gw_signal_native_wait_attempted(record));
  uint64_t physical;
  assert(!syscall(SYS_rt_sigprocmask, SIG_SETMASK, NULL, &physical,
                  sizeof physical));
  assert((physical & private_bits) == private_bits);
  assert(!gw_signal_mask_wait_end(record, 0, EDOM) && errno == EDOM);
  check_mask(1);

  /* The helper is already pending at unmask. It must skip, rather than enter,
   * the sixty-second syscall after returning from that notification. */
  record = gw_signal_mask_wait_current_begin();
  assert(record);
  inject_hidden = 1;
  gw_signal_mask_wait_test_point(2);
  struct timespec long_wait = {.tv_sec = 60};
  int result = gw_signal_wait_result(gateway_sleep(record, &long_wait));
  assert(result == -1 && errno == EINTR);
  assert(!gw_signal_native_wait_attempted(record));
  assert(gw_signal_mask_wait_retry(record, result, errno));
  short_wait = (struct timespec){0};
  assert(!gateway_sleep(record, &short_wait));
  assert(gw_signal_native_wait_attempted(record));
  assert(!gw_signal_mask_wait_end(record, 0, EDOM) && errno == EDOM);
  check_mask(1);

  record = gw_signal_mask_wait_current_begin();
  assert(record);
  frame_edit = 1;
  assert(!syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), SIGUSR1));
  result = gw_signal_wait_result(gateway_sleep(record, &short_wait));
  assert(!result);
  assert(gw_signal_native_wait_attempted(record));
  assert(!gw_signal_mask_wait_end(record, result, EDOM));
  frame_edit = 0;
  check_mask(0);
  assert(!pthread_sigmask(SIG_SETMASK, &blocked, NULL));
}

int main(int argc, char **argv) {
  int native = argc == 2 && !strcmp(argv[1], "--native");
  assert(argc == 1 || native);
  sigset_t inherited;
  assert(!pthread_sigmask(SIG_SETMASK, NULL, &inherited));
  if (!native) {
    assert(!signal_router_init());
  }
  int notifications[GW_SIGNAL_FAULT_LANES];
  for (int i = 0; i < GW_SIGNAL_FAULT_LANES; ++i) {
    notifications[i] = SIGRTMIN + i;
    if (!native) {
      private_bits |= UINT64_C(1) << (notifications[i] - 1);
    }
  }
  if (!native) {
    assert(!signal_router_defer_faults(notifications));
  }
  struct exchange exchange = {.host_tid = syscall(SYS_gettid)};
  if (!native) {
    recipient = signal_acquire(&exchange);
    assert(recipient && recipient->deferred_active);
  }
  struct sigaction action = {.sa_sigaction = receive,
                             .sa_flags = SA_SIGINFO | SA_NODEFER};
  sigemptyset(&action.sa_mask);
  signal_router_backend registration =
      native ? (signal_router_backend)dlsym(RTLD_NEXT, "sigaction") : sigaction;
  assert(registration);
  struct sigaction old_trap, old_pipe;
  assert(!registration(SIGTRAP, &action, &old_trap));
  assert(!registration(SIGUSR1, &action, &old_pipe));
  assert(!(signal_router_reserved() & (UINT64_C(1) << (SIGUSR1 - 1))));
  blocked = inherited;
  sigaddset(&blocked, SIGTRAP);
  sigdelset(&blocked, SIGUSR1);
  temporary = blocked;
  sigdelset(&temporary, SIGTRAP);
  assert(!pthread_sigmask(SIG_SETMASK, &blocked, NULL));
  /* Ordinary Host deliveries must not expose private kernel mask bits in
   * ucontext, including when no deferred original is being delivered. */
  assert(!raise(SIGUSR1));
  for (int desired = 0; desired <= 1; ++desired) {
    assert(
        !pthread_sigmask(SIG_SETMASK, desired ? &temporary : &blocked, NULL));
    frame_edit = desired + 1;
    assert(!raise(SIGUSR1));
    check_mask(desired);
  }
  frame_edit = 0;
  assert(!pthread_sigmask(SIG_SETMASK, &blocked, NULL));
  epoll_fd = epoll_create1(EPOLL_CLOEXEC);
  assert(epoll_fd >= 0);
  unsigned exercised = 0, skipped = 0;
  int count = 5;
#ifdef __ANDROID__
  count = 10;
#endif
  for (int i = 0; i < count; ++i) {
    unsigned done = exercise(i);
    exercised += done;
    skipped += !done;
  }
  if (!native) {
    transparent_waits(count);
    readiness_after_retry();
    select_native_comparison();
    native_wait_gateway();
  }
  wait_jumps();
  assert(!close(epoll_fd));
  if (recipient) {
    assert(!signal_prepare_release(recipient, 0));
    signal_release(recipient);
  }
  assert(!registration(SIGTRAP, &old_trap, NULL));
  assert(!registration(SIGUSR1, &old_pipe, NULL));
  if (!native) {
    assert(!signal_router_restore());
  }
  assert(!pthread_sigmask(SIG_SETMASK, &inherited, NULL));
  printf(
      "{\"result\":\"PASS\",\"atomic_waits\":true,\"logical_context\":true,"
      "\"null_and_blocked_masks\":true,\"native_jump_masks\":true,"
      "\"frame_mask_edits\":true%s}\n",
      native ? ""
             : ",\"hidden_notification_retry\":true,"
               "\"original_deadline\":true,\"host_delivery_interrupts\":true,"
               "\"null_and_plain_wait_retry\":true,"
               "\"hidden_nested_wait\":true,\"select_timeout_semantics\":true,"
               "\"readiness_after_retry\":true,\"native_wait_gateway\":true");
  fprintf(stderr, "waits_exercised=%u waits_skipped=%u jumps=%d\n", exercised,
          skipped, jumps);
}
