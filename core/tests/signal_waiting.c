#define _GNU_SOURCE
#include "signals.h"
#include "signal-router.h"
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
#include <ucontext.h>

static struct signal_slot *recipient;
static volatile sig_atomic_t deliveries, jumps, jump_mode, frame_edit;
static sigjmp_buf checkpoint;
static sigset_t blocked, temporary;
static int epoll_fd;
static uint64_t private_bits;

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
  } else if (frame_edit) {
    if (frame_edit == 1) {
      sigdelset(&frame->uc_sigmask, SIGTRAP);
    } else {
      sigaddset(&frame->uc_sigmask, SIGTRAP);
    }
  }
  errno = saved_errno;
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
      "\"frame_mask_edits\":true}\n");
  fprintf(stderr, "waits_exercised=%u waits_skipped=%u jumps=%d\n", exercised,
          skipped, jumps);
}
