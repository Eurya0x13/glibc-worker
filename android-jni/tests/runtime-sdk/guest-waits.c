#define _GNU_SOURCE
#include "nonlocal_probe.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/poll.h>
#include <sys/select.h>
#include <sys/signalfd.h>
#include <sys/syscall.h>
#include <unistd.h>

enum { WAIT_DATA_SIGNAL = 54, WAIT_INTERRUPT_SIGNAL = 53 };

static int host_pending(void) {
  uint64_t mask = 0;
  return !syscall(SYS_rt_sigpending, &mask, sizeof mask) &&
         (mask & (UINT64_C(1) << (SDK_HOST_ESCAPE_SIGNAL - 1)));
}

static int queue_data(void) {
  siginfo_t information = {.si_signo = WAIT_DATA_SIGNAL, .si_code = SI_QUEUE};
  information.si_pid = getpid();
  information.si_uid = getuid();
  information.si_value.sival_int = 719;
  return syscall(SYS_rt_tgsigqueueinfo, getpid(), syscall(SYS_gettid),
                 WAIT_DATA_SIGNAL, &information);
}

static int consume_data(int mode) {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SDK_HOST_ESCAPE_SIGNAL);
  sigaddset(&set, WAIT_DATA_SIGNAL);
  if (queue_data()) {
    return -60;
  }
  siginfo_t information = {0};
  int result;
  if (mode == 62) {
    result = sigwaitinfo(&set, &information);
  } else {
    result = sigwait(&set, &information.si_signo);
    if (!result) {
      result = information.si_signo;
    }
  }
  return result == WAIT_DATA_SIGNAL &&
                 (mode != 62 || (information.si_code == SI_QUEUE &&
                                 information.si_value.sival_int == 719 &&
                                 information.si_pid == getpid() &&
                                 information.si_uid == getuid()))
             ? 73
             : -61;
}

static int temporary_wait(int mode, const sigset_t *set, int epoll,
                          const struct timespec *timeout) {
  struct epoll_event event;
  switch (mode) {
    case 64:
    case 69:
      return ppoll(NULL, 0, timeout, set);
    case 65:
    case 70:
      return pselect(0, NULL, NULL, NULL, timeout, set);
    case 66:
    case 71:
      return epoll_pwait(epoll, &event, 1, 20, set);
    case 67:
    case 72:
      return epoll_pwait2(epoll, &event, 1, timeout, set);
    default:
      return sigsuspend(set);
  }
}

struct interrupt_sender {
  atomic_int stop;
  long target;
};

static void *send_interrupts(void *argument) {
  struct interrupt_sender *sender = argument;
  const struct timespec delay = {.tv_nsec = 1000000};
  while (!atomic_load_explicit(&sender->stop, memory_order_acquire)) {
    if (syscall(SYS_tgkill, getpid(), sender->target, WAIT_INTERRUPT_SIGNAL)) {
      _exit(164);
    }
    nanosleep(&delay, NULL);
  }
  return NULL;
}

static int wait_call(int mode, const sigset_t *set, int epoll) {
  const struct timespec timeout = {.tv_nsec = 20000000};
  int interrupt = mode >= 68 && mode <= 72;
  struct interrupt_sender sender = {.target = syscall(SYS_gettid)};
  pthread_t thread;
  if (interrupt && pthread_create(&thread, NULL, send_interrupts, &sender)) {
    return -62;
  }
  errno = 0;
  int result = temporary_wait(mode, set, epoll, &timeout), error = errno;
  if (interrupt) {
    atomic_store_explicit(&sender.stop, 1, memory_order_release);
    if (pthread_join(thread, NULL)) {
      _exit(165);
    }
  }
  if ((mode == 67 || mode == 72) && result == -1 && error == ENOSYS) {
    return 74;
  }
  return interrupt ? (result == -1 && error == EINTR ? 73 : -63)
                   : (result == 0 ? 73 : -64);
}

static int error_paths(const sigset_t *set) {
  const struct timespec invalid = {.tv_nsec = -1}, immediate = {0};
  struct epoll_event event;
  if (ppoll(NULL, 0, &invalid, set) != -1 || errno != EINVAL ||
      pselect(-1, NULL, NULL, NULL, &immediate, set) != -1 || errno != EINVAL ||
      epoll_pwait(-1, &event, 1, 0, set) != -1 || errno != EBADF) {
    return -65;
  }
  return 73;
}

static int descriptor_wait(int mode) {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SDK_HOST_ESCAPE_SIGNAL);
  int descriptor = signalfd(-1, &set, SFD_CLOEXEC | SFD_NONBLOCK);
  if (descriptor < 0) {
    return -78;
  }
  struct signalfd_siginfo information;
  int result = 73;
  if (mode == 78) {
    sigaddset(&set, WAIT_DATA_SIGNAL);
    if (signalfd(descriptor, &set, SFD_CLOEXEC | SFD_NONBLOCK) != descriptor ||
        queue_data() ||
        read(descriptor, &information, sizeof information) !=
            sizeof information ||
        information.ssi_signo != WAIT_DATA_SIGNAL ||
        information.ssi_code != SI_QUEUE || information.ssi_int != 719 ||
        information.ssi_pid != (unsigned)getpid() ||
        information.ssi_uid != getuid()) {
      result = -79;
    }
  }
  errno = 0;
  if (read(descriptor, &information, sizeof information) != -1 ||
      errno != EAGAIN) {
    result = -80;
  }
  if (close(descriptor)) {
    _exit(172);
  }
  return result;
}

static void *native_wait(void *argument) {
  int mode = *(int *)argument;
  sigset_t set, previous;
  sigemptyset(&set);
  sigaddset(&set, WAIT_DATA_SIGNAL);
  if (pthread_sigmask(SIG_BLOCK, &set, &previous) || queue_data()) {
    return (void *)(intptr_t)-66;
  }
  sigset_t pending;
  int result = !sigpending(&pending) && sigismember(&pending, WAIT_DATA_SIGNAL)
                   ? consume_data(62)
                   : -67;
  /* consume_data queued a second RT instance. Drain it before unblocking. */
  const struct timespec immediate = {0};
  result =
      sigtimedwait(&set, NULL, &immediate) == WAIT_DATA_SIGNAL ? result : -68;
  if (pthread_sigmask(SIG_SETMASK, &previous, NULL)) {
    return (void *)(intptr_t)-69;
  }
  return (void *)(intptr_t)(mode == 75 ? result : -70);
}

struct cancel_wait {
  atomic_int ready;
};

static void *native_cancel_wait(void *argument) {
  struct cancel_wait *state = argument;
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, WAIT_DATA_SIGNAL);
  if (pthread_sigmask(SIG_BLOCK, &set, NULL)) {
    _exit(166);
  }
  atomic_store_explicit(&state->ready, 1, memory_order_release);
  sigwaitinfo(&set, NULL);
  return NULL;
}

static int native_thread_probe(int mode) {
  pthread_t thread;
  void *result;
  struct cancel_wait state = {0};
  if (pthread_create(&thread, NULL,
                     mode == 75 ? native_wait : native_cancel_wait,
                     mode == 75 ? (void *)&mode : (void *)&state)) {
    return -71;
  }
  if (mode == 76) {
    while (!atomic_load_explicit(&state.ready, memory_order_acquire)) {
      sched_yield();
    }
    if (pthread_cancel(thread)) {
      _exit(167);
    }
  }
  if (pthread_join(thread, &result)) {
    return -72;
  }
  return mode == 75 ? (int)(intptr_t)result
                    : (result == PTHREAD_CANCELED ? 73 : -73);
}

struct abandoned_wait {
  pthread_t sender;
  long target;
  sigset_t previous;
  atomic_int observed;
};

static __thread struct abandoned_wait abandoned;

static void *interrupt_suspended_thread(void *argument) {
  struct abandoned_wait *state = argument;
  char path[96], buffer[256];
  snprintf(path, sizeof path, "/proc/self/task/%ld/syscall", state->target);
  int descriptor = open(path, O_RDONLY | O_CLOEXEC);
  const struct timespec delay = {.tv_nsec = 100000};
  if (descriptor >= 0) {
    for (int attempt = 0; attempt < 10000; ++attempt) {
      ssize_t size = pread(descriptor, buffer, sizeof buffer - 1, 0);
      if (size > 0) {
        buffer[size] = 0;
        char *end;
        long number = strtol(buffer, &end, 10);
        if (end != buffer && number == SYS_rt_sigsuspend) {
          atomic_store_explicit(&state->observed, 1, memory_order_release);
          break;
        }
      }
      nanosleep(&delay, NULL);
    }
    close(descriptor);
  }
  /* A failed observation still wakes the test so it fails without hanging.
   * Only a positive kernel-wait observation counts as an unwind test. */
  if (syscall(SYS_tgkill, getpid(), state->target, WAIT_INTERRUPT_SIGNAL)) {
    _exit(173);
  }
  return NULL;
}

static int unwind_wait(int mode) {
  struct abandoned_wait *state = &abandoned;
  if (mode == 80) {
    if (pthread_join(state->sender, NULL) ||
        pthread_sigmask(SIG_SETMASK, &state->previous, NULL)) {
      _exit(174);
    }
    return atomic_load_explicit(&state->observed, memory_order_acquire) ? 73
                                                                        : -81;
  }
  atomic_store(&state->observed, 0);
  state->target = syscall(SYS_gettid);
  if (pthread_sigmask(SIG_SETMASK, NULL, &state->previous)) {
    return -82;
  }
  sigset_t temporary = state->previous;
  sigaddset(&temporary, WAIT_INTERRUPT_SIGNAL);
  if (pthread_create(&state->sender, NULL, interrupt_suspended_thread, state)) {
    return -83;
  }
  sigsuspend(&temporary);
  return -84;
}

int sdk_guest_wait_probe(int mode) {
  if (mode == 79 || mode == 80) {
    return unwind_wait(mode);
  }
  if (mode == 75 || mode == 76) {
    return native_thread_probe(mode);
  }
  sigset_t changed, inherited, before, after, temporary;
  sigemptyset(&changed);
  sigaddset(&changed, WAIT_DATA_SIGNAL);
  if (pthread_sigmask(SIG_BLOCK, &changed, &inherited)) {
    return -74;
  }
  sigemptyset(&changed);
  sigaddset(&changed, SDK_HOST_ESCAPE_SIGNAL);
  if (pthread_sigmask(SIG_UNBLOCK, &changed, NULL) ||
      pthread_sigmask(SIG_SETMASK, NULL, &before)) {
    _exit(168);
  }
  temporary = before;
  sigaddset(&temporary, WAIT_INTERRUPT_SIGNAL);
  int epoll = epoll_create1(EPOLL_CLOEXEC);
  if (epoll < 0 || !host_pending()) {
    _exit(169);
  }
  int result;
  if (mode == 60) {
    sigset_t pending;
    result =
        !sigpending(&pending) && !sigismember(&pending, SDK_HOST_ESCAPE_SIGNAL)
            ? 73
            : -75;
  } else if (mode == 61) {
    const struct timespec immediate = {0};
    errno = 0;
    result = sigtimedwait(&changed, NULL, &immediate) == -1 && errno == EAGAIN
                 ? 73
                 : -76;
  } else if (mode == 62 || mode == 63) {
    result = consume_data(mode);
  } else if (mode == 73) {
    result = error_paths(&temporary);
  } else if (mode == 74) {
    result = wait_call(64, NULL, epoll);
  } else if (mode == 77 || mode == 78) {
    result = descriptor_wait(mode);
  } else {
    result = wait_call(mode, &temporary, epoll);
  }
  if (!host_pending() || pthread_sigmask(SIG_SETMASK, NULL, &after) ||
      sigismember(&after, SDK_HOST_ESCAPE_SIGNAL) !=
          sigismember(&before, SDK_HOST_ESCAPE_SIGNAL) ||
      sigismember(&after, WAIT_INTERRUPT_SIGNAL) !=
          sigismember(&before, WAIT_INTERRUPT_SIGNAL)) {
    result = -77;
  }
  if (close(epoll) || pthread_sigmask(SIG_SETMASK, &inherited, NULL)) {
    _exit(170);
  }
  return result;
}
