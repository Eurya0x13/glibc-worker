#define _GNU_SOURCE
#include "signals.h"
#include "signal-router.h"
#include "signal_channel.h"
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <time.h>
#include <stdio.h>
#include <stdatomic.h>
#include <sched.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <ucontext.h>

/* Compare actual handler entry order, masks, selected generations and payloads
 * with this kernel/libc, rather than imposing a POSIX unspecified order.
 * https://github.com/torvalds/linux/blob/v6.12/kernel/signal.c
 * See next_signal(), dequeue_signal() and get_signal(). */
struct observation {
  int number, value, generation, reserved;
  uint64_t execution, returned;
};

struct result {
  int count, unavailable;
  struct observation events[16];
};

static struct result observed;
static struct signal_slot *recipient;
static signal_router_backend registration;
static int routed, hook_mode, hook_done, nested, epoll_fd;
static uint64_t private_bits;
static struct exchange carrier;
static sigset_t all, empty;
static const int numbers[] = {SIGILL,  SIGTRAP, SIGBUS,  SIGFPE,
                              SIGSEGV, SIGSYS,  SIGUSR1, SIGALRM};

#ifndef __ANDROID__
static ucontext_t outside, suspended, resumer;
static volatile sig_atomic_t context_mode, transferred;
#endif

static uint64_t bits(const sigset_t *mask) {
  uint64_t result;
  memcpy(&result, mask, sizeof result);
  return result;
}

static void send_native(int number, int value, int shared) {
  siginfo_t info = {.si_signo = number, .si_code = SI_QUEUE};
  info.si_pid = getpid();
  info.si_uid = getuid();
  info.si_value.sival_int = value;
  assert(!(shared ? syscall(SYS_rt_sigqueueinfo, getpid(), number, &info)
                  : syscall(SYS_rt_tgsigqueueinfo, getpid(),
                            syscall(SYS_gettid), number, &info)));
}

static void send_packet(int number, int value, int shared) {
  if (routed && !shared &&
      (gw_signal_fault_bits() & (UINT64_C(1) << (number - 1)))) {
    siginfo_t info = {.si_signo = number, .si_code = SI_QUEUE};
    info.si_pid = getpid();
    info.si_uid = getuid();
    info.si_value.sival_int = value;
    assert(!signal_host_defer(recipient, &info));
  } else {
    send_native(number, value, shared);
  }
}

static void receive_new(int, siginfo_t *, void *);

void signal_router_pending_observe(int number) {
  if (number != SIGILL || !hook_mode || hook_done) {
    return;
  }
  hook_done = 1;
  if (hook_mode == 2) {
    struct sigaction ignore = {.sa_handler = SIG_IGN, .sa_mask = all};
    assert(!registration(number, &ignore, NULL));
    struct sigaction replacement = {
        .sa_sigaction = receive_new, .sa_mask = all, .sa_flags = SA_SIGINFO};
    assert(!registration(number, &replacement, NULL));
  }
  send_native(number, 9911, 0);
}

static void record_delivery(int number, siginfo_t *info, void *context,
                            int generation) {
  if (!routed) {
    signal_router_pending_observe(number);
  }
  assert(info->si_signo == number && info->si_code == SI_QUEUE);
  assert(info->si_pid == getpid() && info->si_uid == getuid());

  int index = observed.count++;
  assert(index < 16);
  sigset_t execution;
  assert(!pthread_sigmask(SIG_SETMASK, NULL, &execution));
  ucontext_t *frame = context;
  if (routed) {
    assert(!(bits(&execution) & private_bits));
    assert(!(bits(&frame->uc_sigmask) & private_bits));
  }

  observed.events[index] = (struct observation){
      .number = number,
      .value = info->si_value.sival_int,
      .generation = generation,
      .execution = bits(&execution) & ~private_bits,
      .returned = bits(&frame->uc_sigmask) & ~private_bits};

#ifndef __ANDROID__
  if (context_mode && number == SIGILL) {
    transferred = 1;
    if (context_mode == 1) {
      assert(!setcontext(&outside));
      assert(0);
    }
    assert(!swapcontext(&suspended, &outside));
  }
#endif

  if (nested && number == SIGILL) {
    nested = 0;
    sigset_t alarm_mask, previous;
    sigemptyset(&alarm_mask);
    sigaddset(&alarm_mask, SIGALRM);
    assert(!pthread_sigmask(SIG_BLOCK, &alarm_mask, &previous));
    send_native(SIGALRM, 7123, 0);
    struct timespec zero = {0};
    assert(ppoll(NULL, 0, &zero, &empty) == -1 && errno == EINTR);
    assert(!pthread_sigmask(SIG_SETMASK, &previous, NULL));
  }
}

static void receive(int number, siginfo_t *info, void *context) {
  record_delivery(number, info, context, 1);
}

static void receive_new(int number, siginfo_t *info, void *context) {
  record_delivery(number, info, context, 2);
}

static int wait_call(int mode) {
  struct timespec timeout = {.tv_nsec = 100000000};
  struct epoll_event event;
  switch (mode) {
    case 0:
      return pthread_sigmask(SIG_SETMASK, &empty, NULL);
    case 1:
      return sigsuspend(&empty);
    case 2:
      return ppoll(NULL, 0, &timeout, &empty);
    case 3:
      return pselect(0, NULL, NULL, NULL, &timeout, &empty);
    case 4:
      return epoll_pwait(epoll_fd, &event, 1, 100, &empty);
    case 5: {
      typedef int (*function_type)(int, struct epoll_event *, int,
                                   const struct timespec *, const sigset_t *);
      function_type function =
          (function_type)dlsym(RTLD_DEFAULT, "epoll_pwait2");
      assert(function);
      return function(epoll_fd, &event, 1, &timeout, &empty);
    }
#ifdef __ANDROID__
    case 6:
      return sigsuspend64((const sigset64_t *)&empty);
    case 7:
      return ppoll64(NULL, 0, &timeout, (const sigset64_t *)&empty);
    case 8:
      return pselect64(0, NULL, NULL, NULL, &timeout,
                       (const sigset64_t *)&empty);
    case 9:
      return epoll_pwait64(epoll_fd, &event, 1, 100,
                           (const sigset64_t *)&empty);
    case 10: {
      typedef int (*function_type)(int, struct epoll_event *, int,
                                   const struct timespec *, const sigset64_t *);
      function_type function =
          (function_type)dlsym(RTLD_DEFAULT, "epoll_pwait2_64");
      assert(function);
      return function(epoll_fd, &event, 1, &timeout,
                      (const sigset64_t *)&empty);
    }
#endif
    default:
      assert(0);
  }
  return -2;
}

static void prepare_case(int variant, int kind, int channel_after) {
  routed = variant;
  sigfillset(&all);
  sigemptyset(&empty);
  assert(!pthread_sigmask(SIG_SETMASK, &all, NULL));
  registration =
      routed ? sigaction : (signal_router_backend)dlsym(RTLD_NEXT, "sigaction");
  assert(registration);
  if (routed) {
    assert(!signal_router_init());
  }

  const int notifications[] = {SIGRTMIN,     SIGRTMIN + 1, SIGRTMIN + 2,
                               SIGRTMIN + 3, SIGRTMIN + 4, SIGRTMIN + 5};
  for (int i = 0; i < 6; ++i) {
    private_bits |= UINT64_C(1) << (notifications[i] - 1);
  }
  if (routed && !channel_after) {
    assert(!gw_host_signal_channel_enable(notifications, 6));
  }
  for (unsigned i = 0; i < sizeof numbers / sizeof *numbers; ++i) {
    struct sigaction action = {.sa_sigaction = receive, .sa_flags = SA_SIGINFO};
    if (kind != 1 && kind != 8 && kind != 9) {
      action.sa_mask = all;
    } else {
      sigemptyset(&action.sa_mask);
      action.sa_flags |= SA_NODEFER;
      if (kind == 8) {
        sigaddset(&action.sa_mask, SIGUSR1);
      }
    }
    assert(!registration(numbers[i], &action, NULL));
  }
  if (routed) {
    if (channel_after == 2) {
      struct sigaction ignore = {.sa_handler = SIG_IGN};
      struct sigaction normal = {.sa_handler = SIG_DFL};
      struct sigaction old_segv, old_bus;
      assert(!registration(SIGSEGV, &ignore, &old_segv));
      assert(!registration(SIGBUS, &normal, &old_bus));
      assert(!gw_host_signal_channel_enable(notifications, 6));
      assert(!registration(SIGSEGV, &old_segv, NULL));
      assert(!registration(SIGBUS, &old_bus, NULL));
    } else if (channel_after) {
      assert(!gw_host_signal_channel_enable(notifications, 6));
    }
    carrier.host_tid = syscall(SYS_gettid);
    recipient = signal_acquire(&carrier);
    assert(recipient);
  }
  epoll_fd = epoll_create1(EPOLL_CLOEXEC);
  assert(epoll_fd >= 0);
}

static void queue_case(int kind) {
  if (kind == 10) {
    send_packet(SIGILL, 1001, 0);
    if (routed) {
      signal_carrier_mask(recipient, 0, 0);
    }
    struct sigaction ignore = {.sa_handler = SIG_IGN}, previous;
    assert(!registration(SIGILL, &ignore, &previous));
    assert(!registration(SIGILL, &previous, NULL));
    if (routed) {
      assert(!signal_host_retirement_busy());
    }
    sigset_t pending;
    assert(!sigpending(&pending));
    assert(!sigismember(&pending, SIGILL));
    send_native(SIGILL, 9911, 0);
  } else if (kind == 7) {
    send_packet(SIGILL, 1001, 0);
    send_native(SIGILL, 1002, 0);
  } else if (kind == 9) {
    nested = 1;
    send_packet(SIGUSR1, 1001, 0);
    send_packet(SIGILL, 1002, 1);
  } else if (kind == 2 || kind == 3) {
    hook_mode = kind - 1;
    send_packet(SIGILL, 1001, 0);
  } else if (kind == 4) {
    send_packet(SIGSEGV, 1001, 0);
    send_packet(SIGILL, 1002, 1);
    send_packet(SIGUSR1, 1003, 0);
  } else if (kind == 5) {
    send_packet(SIGILL, 1001, 0);
    send_packet(SIGILL, 1002, 1);
  } else {
    for (int i = 5; i >= 0; --i) {
      send_packet(numbers[i], 1000 + i, 0);
    }
    send_packet(SIGUSR1, 1006, 0);
    if (kind == 6) {
      nested = 1;
    }
  }
}

static void run_case(int variant, int mode, int kind, int channel_after) {
  prepare_case(variant, kind, channel_after);
  queue_case(kind);

  int result = wait_call(mode);
  int error = errno;
  if (result == -1 && error == ENOSYS) {
    assert(mode == 5 || mode == 10);
    observed.unavailable = 1;
    assert(!pthread_sigmask(SIG_SETMASK, &empty, NULL));
  } else {
    assert(mode ? result == -1 && error == EINTR : !result);
    /* Atomic mask waits restore the blocked pre-wait mask. Drain the remaining
     * original numbers exactly as a native caller would, not via a shadow API.
     */
    assert(!pthread_sigmask(SIG_SETMASK, &empty, NULL));
  }
  assert(observed.count > 0);
  if (routed) {
    assert(!signal_prepare_release(recipient, 0));
    signal_release(recipient);
    assert(!signal_router_restore());
  }
  assert(!close(epoll_fd));
}

static struct result child_case(int variant, int mode, int kind,
                                int channel_after) {
  int pipefd[2];
  assert(!pipe(pipefd));
  pid_t child = fork();
  assert(child >= 0);
  if (!child) {
    close(pipefd[0]);
    alarm(3);
    run_case(variant, mode, kind, channel_after);
    assert(write(pipefd[1], &observed, sizeof observed) == sizeof observed);
    _exit(0);
  }

  close(pipefd[1]);
  struct result result;
  assert(read(pipefd[0], &result, sizeof result) == sizeof result);
  close(pipefd[0]);
  int status;
  assert(waitpid(child, &status, 0) == child);
  assert(WIFEXITED(status) && !WEXITSTATUS(status));
  return result;
}

#ifndef __ANDROID__
static unsigned char wait_stack[65536];

static void context_wait(void) {
  send_packet(SIGILL, 7193, 0);
  assert(sigsuspend(&empty) == -1 && errno == EINTR);
  assert(context_mode == 0 && transferred == 2);
  assert(!setcontext(&resumer));
  assert(0);
}

static void context_case(int variant, int mode) {
  prepare_case(variant, 0, 0);
  context_mode = mode;
  ucontext_t fiber;
  assert(!getcontext(&fiber));
  fiber.uc_stack = (stack_t){.ss_sp = wait_stack, .ss_size = sizeof wait_stack};
  fiber.uc_link = NULL;
  makecontext(&fiber, context_wait, 0);
  assert(!swapcontext(&outside, &fiber));
  assert(transferred);

  /* The old signal frame and wait have independent retained stack storage.
   * An unrelated wait uses/retires another row on the original caller stack.
   * No jump interposer is linked into this fixture. */
  context_mode = 0;
  send_packet(SIGTRAP, 7194, 0);
  struct timespec zero = {0};
  assert(ppoll(NULL, 0, &zero, &empty) == -1 && errno == EINTR);
  if (mode == 2 && transferred == 1) {
    transferred = 2;
    assert(!swapcontext(&resumer, &suspended));
  }
  assert(observed.count == 2);
  if (routed) {
    assert(!signal_prepare_release(recipient, 0));
    signal_release(recipient);
    assert(!signal_router_restore());
  }
  assert(!close(epoll_fd));
}

static void contexts(void) {
  for (int variant = 0; variant <= 1; ++variant) {
    for (int mode = 1; mode <= 2; ++mode) {
      pid_t child = fork();
      assert(child >= 0);
      if (!child) {
        alarm(3);
        context_case(variant, mode);
        _exit(0);
      }
      int status;
      assert(waitpid(child, &status, 0) == child);
      assert(WIFEXITED(status) && !WEXITSTATUS(status));
    }
  }
}

static _Atomic int cancel_ready, cancel_done;
static int cancel_stage, cancel_mode;

void gw_signal_mask_wait_test_point(int stage) {
  if (stage == cancel_stage) {
    atomic_store_explicit(&cancel_ready, 1, memory_order_release);
    while (!atomic_load_explicit(&cancel_done, memory_order_acquire)) {
      sched_yield();
    }
  }
}

static void *cancel_wait(void *unused) {
  (void)unused;
  if (cancel_stage == 3) {
    atomic_store_explicit(&cancel_ready, 1, memory_order_release);
  }
  wait_call(cancel_mode);
  assert(!"atomic mask wait suppressed pthread cancellation");
  return NULL;
}

static void cancellation_case(int mode, int stage) {
  sigemptyset(&empty);
  sigfillset(&all);
  assert(!signal_router_init());
  const int notifications[] = {SIGRTMIN,     SIGRTMIN + 1, SIGRTMIN + 2,
                               SIGRTMIN + 3, SIGRTMIN + 4, SIGRTMIN + 5};
  assert(!gw_host_signal_channel_enable(notifications, 6));
  epoll_fd = epoll_create1(EPOLL_CLOEXEC);
  assert(epoll_fd >= 0);
  cancel_mode = mode;
  cancel_stage = stage;
  pthread_t thread;
  assert(!pthread_create(&thread, NULL, cancel_wait, NULL));
  while (!atomic_load_explicit(&cancel_ready, memory_order_acquire)) {
    sched_yield();
  }
  if (stage == 3) {
    struct timespec delay = {.tv_nsec = 10000000};
    assert(!nanosleep(&delay, NULL));
  }
  assert(!pthread_cancel(thread));
  atomic_store_explicit(&cancel_done, 1, memory_order_release);
  void *result;
  assert(!pthread_join(thread, &result) && result == PTHREAD_CANCELED);
  assert(!close(epoll_fd));
  assert(!signal_router_restore());
}

static void cancellations(void) {
  for (int mode = 1; mode <= 5; ++mode) {
    for (int stage = 1; stage <= 3; ++stage) {
      pid_t child = fork();
      assert(child >= 0);
      if (!child) {
        alarm(3);
        cancellation_case(mode, stage);
        _exit(0);
      }
      int status;
      assert(waitpid(child, &status, 0) == child);
      if (!WIFEXITED(status) || WEXITSTATUS(status)) {
        fprintf(stderr, "cancellation mode=%d stage=%d status=%d\n", mode,
                stage, status);
        assert(0);
      }
    }
  }
}
#endif

int main(void) {
  struct rlimit limit = {0};
  assert(!setrlimit(RLIMIT_CORE, &limit));
  int modes = 6;
#ifdef __ANDROID__
  modes = 11;
#endif
  unsigned pairs = 0, unavailable = 0;
  for (int mode = 0; mode < modes; ++mode) {
    unsigned skipped = 0;
    for (int kind = 0; kind < 11; ++kind) {
      for (int after = 0; after <= 2; ++after) {
        struct result native = child_case(0, mode, kind, after);
        struct result translated = child_case(1, mode, kind, after);
        ++pairs;
        skipped += native.unavailable;
        if (memcmp(&native, &translated, sizeof native)) {
          fprintf(stderr, "handler mismatch mode=%d kind=%d after=%d\n", mode,
                  kind, after);
          for (int i = 0; i < native.count || i < translated.count; ++i) {
            struct observation a = native.events[i], b = translated.events[i];
            fprintf(stderr, "%d: %d/%d %d/%d %d/%d %llx/%llx %llx/%llx\n", i,
                    a.number, b.number, a.value, b.value, a.generation,
                    b.generation, (unsigned long long)a.execution,
                    (unsigned long long)b.execution,
                    (unsigned long long)a.returned,
                    (unsigned long long)b.returned);
          }
          return 1;
        }
      }
    }
    if (skipped) {
      fprintf(stderr, "native_wait_unavailable mode=%d pairs=%u\n", mode,
              skipped);
      unavailable += skipped;
    }
  }
  fprintf(stderr, "handler_pairs=%u unavailable_wait_pairs=%u\n", pairs,
          unavailable);
#ifndef __ANDROID__
  cancellations();
  contexts();
#endif
  puts(
      "{\"result\":\"PASS\",\"handler_priority\":true,"
      "\"temporary_masks\":true,\"selected_generation\":true,"
      "\"fresh_same_number\":true,\"thread_before_shared\":true,"
      "\"nested_waits\":true,\"preexisting_registrations\":true,"
#ifdef __ANDROID__
      "\"pthread_cancel_unavailable\":true}"
#else
      "\"cancellation_windows\":true,\"native_saved_wait_context\":true}"
#endif
  );
}
