#define _GNU_SOURCE
#include "nonlocal-mask.h"
#include "signal-router.h"
#include "signals.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#ifndef __ANDROID__
static _Thread_local int cancel_before_entry;
static _Atomic int cleaned;
#endif
static volatile sig_atomic_t jump_kind, jumped;
static sigjmp_buf jump_buffer;
#ifndef __ANDROID__
static ucontext_t saved_context;
static ucontext_t handler_context, outside_context;
static volatile sig_atomic_t context_resumes;
#endif
static _Atomic long waiting_tid;
static int visible_before_entry, notify_sender, delay_hidden;
static volatile sig_atomic_t hidden_seen;

void gw_signal_mask_wait_test_point(int stage) {
  if (stage == 2 && visible_before_entry) {
    visible_before_entry = 0;
    assert(!syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), SIGUSR1));
  }
#ifndef __ANDROID__
  if (stage == 2 && cancel_before_entry) {
    cancel_before_entry = 0;
    assert(!pthread_cancel(pthread_self()));
  }
#else
  (void)stage;
#endif
}

void gw_signal_hidden_wait_test_point(void) {
  ++hidden_seen;
  if (delay_hidden) {
    struct timespec work = {.tv_nsec = 150000000};
    assert(!syscall(SYS_clock_nanosleep, CLOCK_MONOTONIC, 0, &work, NULL));
  }
}

static void assert_deferred(void) {
#ifndef __ANDROID__
  int previous;
  assert(!pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, &previous));
  assert(previous == PTHREAD_CANCEL_DEFERRED);
#endif
}

static void receive(int number) {
  assert(number == SIGUSR1);
  assert_deferred();
  jumped = 1;
  if (jump_kind == 1) {
    siglongjmp(jump_buffer, 1);
  }
  if (jump_kind == 3) {
    struct timespec work = {.tv_nsec = 150000000};
    assert(!syscall(SYS_clock_nanosleep, CLOCK_MONOTONIC, 0, &work, NULL));
    /* Leave a helper pending for the unmasked rearm window after this Host
     * handler returns. Its work must count against the sleep; the visible
     * pre-entry handler's own duration must not shorten an unstarted sleep. */
    gw_nonlocal_mask_block();
    assert(!syscall(SYS_tgkill, getpid(), syscall(SYS_gettid),
                    signal_router_fault_notifications()[0]));
    return;
  }
#ifndef __ANDROID__
  if (jump_kind == 4) {
    assert(!swapcontext(&handler_context, &outside_context));
    assert(context_resumes == 1);
    assert(!swapcontext(&handler_context, &outside_context));
    assert(context_resumes == 2);
    return;
  }
  assert(jump_kind == 2);
  assert(!setcontext(&saved_context));
#endif
  _exit(111);
}

static void wait_until_sleeping(void) {
  struct timespec start, now;
  assert(!clock_gettime(CLOCK_MONOTONIC, &start));
  for (;;) {
    long tid = atomic_load_explicit(&waiting_tid, memory_order_acquire);
    if (tid) {
      char path[96], text[256];
      snprintf(path, sizeof path, "/proc/self/task/%ld/syscall", tid);
      int fd = open(path, O_RDONLY | O_CLOEXEC);
      assert(fd >= 0);
      ssize_t size = read(fd, text, sizeof text - 1);
      assert(!close(fd));
      if (size > 0) {
        text[size] = 0;
        long syscall_number;
        if (sscanf(text, "%ld", &syscall_number) == 1 &&
            syscall_number == SYS_clock_nanosleep) {
          return;
        }
      }
    }
    assert(!clock_gettime(CLOCK_MONOTONIC, &now));
    assert(now.tv_sec - start.tv_sec < 5);
    sched_yield();
  }
}

static void *send_signal(void *unused) {
  (void)unused;
  wait_until_sleeping();
  int number = notify_sender ? signal_router_fault_notifications()[0] : SIGUSR1;
  assert(!syscall(SYS_tgkill, getpid(), atomic_load(&waiting_tid), number));
  return NULL;
}

static void sleep_notifications(void) {
  jump_kind = 3;
  hidden_seen = jumped = 0;
  visible_before_entry = 1;
  struct timespec start, end, request = {.tv_nsec = 100000000};
  assert(!clock_gettime(CLOCK_MONOTONIC, &start));
  assert(!nanosleep(&request, NULL));
  assert(!clock_gettime(CLOCK_MONOTONIC, &end));
  long elapsed =
      (end.tv_sec - start.tv_sec) * 1000000000 + end.tv_nsec - start.tv_nsec;
  assert(jumped && !visible_before_entry && hidden_seen);
  assert(elapsed >= 230000000);
  assert_deferred();

  notify_sender = delay_hidden = 1;
  hidden_seen = 0;
  atomic_store(&waiting_tid, syscall(SYS_gettid));
  pthread_t sender;
  assert(!pthread_create(&sender, NULL, send_signal, NULL));
  struct timespec remaining = {.tv_sec = 7193};
  errno = EDOM;
  assert(!nanosleep(&request, &remaining));
  assert(errno == EDOM && remaining.tv_sec == 7193 && hidden_seen == 1);
  assert(!pthread_join(sender, NULL));
  notify_sender = delay_hidden = 0;
}

static void jump_from_sleep(int kind) {
  jump_kind = kind;
  jumped = 0;
  atomic_store(&waiting_tid, syscall(SYS_gettid));
  pthread_t sender;
  assert(!pthread_create(&sender, NULL, send_signal, NULL));

  if (kind == 1) {
    if (!sigsetjmp(jump_buffer, 1)) {
      struct timespec duration = {.tv_sec = 60};
      nanosleep(&duration, NULL);
      assert(!"signal jump did not escape sleep");
    }
  }
#ifndef __ANDROID__
  else {
    assert(!getcontext(&saved_context));
    if (!jumped) {
      struct timespec duration = {.tv_sec = 60};
      nanosleep(&duration, NULL);
      assert(!"signal context did not escape sleep");
    }
  }
#endif

  assert(!pthread_join(sender, NULL));
  assert(jumped);
  assert_deferred();
  sigset_t mask;
  assert(!pthread_sigmask(SIG_SETMASK, NULL, &mask));
  assert(!sigismember(&mask, SIGUSR1));
  struct timespec zero = {0};
  assert(!nanosleep(&zero, NULL));
  assert_deferred();
}

#ifndef __ANDROID__
static void outside_handler(void) {
  ++context_resumes;
  assert_deferred();
  struct timespec zero = {0};
  assert(!nanosleep(&zero, NULL));
  assert(!setcontext(&handler_context));
  _exit(111);
}

static void saved_handler(void) {
  const size_t stack_size = 65536;
  void *stack = mmap(NULL, stack_size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(stack != MAP_FAILED);
  assert(!getcontext(&outside_context));
  outside_context.uc_stack = (stack_t){.ss_sp = stack, .ss_size = stack_size};
  outside_context.uc_link = NULL;
  makecontext(&outside_context, outside_handler, 0);
  jump_kind = 4;
  context_resumes = 0;
  atomic_store(&waiting_tid, syscall(SYS_gettid));
  pthread_t sender;
  assert(!pthread_create(&sender, NULL, send_signal, NULL));

  struct timespec duration = {.tv_sec = 60}, remaining;
  assert(nanosleep(&duration, &remaining) == -1 && errno == EINTR);
  assert(!pthread_join(sender, NULL));
  assert(context_resumes == 2 && remaining.tv_sec <= duration.tv_sec);
  assert_deferred();
  struct timespec zero = {0};
  assert(!nanosleep(&zero, NULL));
  assert(!munmap(stack, stack_size));
}

static void cleanup_recipient(void *recipient) {
  assert_deferred();
  sigset_t mask;
  assert(!pthread_sigmask(SIG_SETMASK, NULL, &mask));
  assert(sigismember(&mask, SIGUSR2));
  assert(!sigismember(&mask, SIGUSR1));
  assert(!signal_prepare_release(recipient, 0));
  signal_release(recipient);
  atomic_fetch_add(&cleaned, 1);
}

static void *cancelled_sleeper(void *argument) {
  cancel_before_entry = argument != NULL;
  struct exchange exchange = {.host_tid = syscall(SYS_gettid)};
  struct signal_slot *recipient = signal_acquire(&exchange);
  assert(recipient);
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGUSR2);
  assert(!pthread_sigmask(SIG_BLOCK, &mask, NULL));

  pthread_cleanup_push(cleanup_recipient, recipient);
  atomic_store_explicit(&waiting_tid, exchange.host_tid, memory_order_release);
  struct timespec duration = {.tv_sec = 60};
  nanosleep(&duration, NULL);
  assert(!"cancelled sleep returned");
  pthread_cleanup_pop(1);
  return NULL;
}

static void cancellation(void) {
  for (int before_entry = 0; before_entry < 2; ++before_entry) {
    atomic_store(&waiting_tid, 0);
    pthread_t target;
    assert(!pthread_create(&target, NULL, cancelled_sleeper,
                           before_entry ? &target : NULL));
    if (!before_entry) {
      wait_until_sleeping();
      assert(!pthread_cancel(target));
    }
    void *result;
    assert(!pthread_join(target, &result));
    assert(result == PTHREAD_CANCELED);
  }
  assert(atomic_load(&cleaned) == 2);
}
#endif

int main(void) {
  assert(!signal_router_init());
  int notifications[GW_SIGNAL_FAULT_LANES];
  for (int i = 0; i < GW_SIGNAL_FAULT_LANES; ++i) {
    notifications[i] = SIGRTMIN + i;
  }
  assert(!signal_router_defer_faults(notifications));
  struct exchange exchange = {.host_tid = syscall(SYS_gettid)};
  struct signal_slot *recipient = signal_acquire(&exchange);
  assert(recipient);
  struct sigaction action = {.sa_handler = receive, .sa_flags = SA_RESTART};
  assert(!sigaction(SIGUSR1, &action, NULL));
  jump_from_sleep(1);
#ifndef __ANDROID__
  jump_from_sleep(2);
#endif
  sleep_notifications();
#ifndef __ANDROID__
  saved_handler();
  cancellation();
#endif
  assert(!signal_prepare_release(recipient, 0));
  signal_release(recipient);
  assert(!signal_router_restore());
  puts(
      "{\"result\":\"PASS\",\"sleep_siglongjmp\":true,"
      "\"sleep_after_jump\":true,"
      "\"sleep_actual_block_notification\":true,\"sleep_preentry_rearm\":true,"
#ifdef __ANDROID__
      "\"sleep_setcontext\":\"unavailable\","
      "\"sleep_saved_handler_context\":\"unavailable\","
      "\"pthread_cancel\":\"unavailable\"}");
#else
      "\"sleep_setcontext\":true,\"pthread_cancel\":true,\"cancel_before_"
      "entry\":true,"
      "\"cancel_mask_restored\":true,\"sleep_saved_handler_context\":true}");
#endif
}
