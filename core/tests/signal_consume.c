#define _GNU_SOURCE
#include "nonlocal.h"
#include "signals.h"
#include "signal-router.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sys/uio.h>
#include <sys/wait.h>

static sigset_t wanted;
static struct signal_slot *recipient;
static int descriptor;
static volatile sig_atomic_t action, inject, deliveries;
static sigjmp_buf outside;
static void drained(void);

static void queue(int value) {
  siginfo_t information = {.si_signo = SIGTRAP, .si_code = SI_QUEUE};
  information.si_pid = getpid();
  information.si_uid = getuid();
  information.si_value.sival_int = value;
  uint64_t previous = gw_nonlocal_mask_block();
  assert(!signal_host_defer(recipient, &information));
  gw_nonlocal_mask_restore(previous);
}

static void verify(const struct signalfd_siginfo *information, int value) {
  assert(information->ssi_signo == SIGTRAP &&
         information->ssi_code == SI_QUEUE);
  assert(information->ssi_pid == (unsigned)getpid());
  assert(information->ssi_uid == getuid() && information->ssi_int == value);
}

static void receive(int number) {
  assert(number == SIGUSR1);
  ++deliveries;
  sigset_t pending;
  assert(!sigpending(&pending));
  assert(!sigismember(&pending, SIGTRAP));
  /* All kernel/helper ownership is committed before a handler can run. */
  assert(!signal_host_retirement_busy());
  if (action == 1) {
    queue(8200);
    return;
  }
  if (action == 2) {
    queue(8200);
    siglongjmp(outside, 19);
  }
  if (action == 3) {
    queue(8200);
    struct signalfd_siginfo inner;
    assert(read(descriptor, &inner, sizeof inner) == sizeof inner);
    verify(&inner, 8200);
    queue(8300);
    sigjmp_buf local;
    if (!sigsetjmp(local, 1)) {
      siglongjmp(local, 17);
    }
    return;
  }
  if (action == 5) {
    struct sigaction ignore = {.sa_handler = SIG_IGN}, previous;
    sigemptyset(&ignore.sa_mask);
    assert(!sigaction(SIGTRAP, &ignore, &previous));
    assert(!sigaction(SIGTRAP, &previous, NULL));
    queue(8200);
    return;
  }
  assert(action == 4);
  siglongjmp(outside, 23);
}

void gw_signal_consume_test_point(void) {
  if (inject == 3) {
    inject = 0;
    struct sigaction ignore = {.sa_handler = SIG_IGN}, previous;
    sigemptyset(&ignore.sa_mask);
    assert(!sigaction(SIGTRAP, &ignore, &previous));
    assert(!sigaction(SIGTRAP, &previous, NULL));
    return;
  }
#ifndef __ANDROID__
  if (inject == 2) {
    inject = 0;
    assert(!pthread_cancel(pthread_self()));
    pthread_testcancel();
    assert(!"cancellation did not leave the native consumer");
  }
#endif
  if (inject) {
    inject = 0;
    assert(!raise(SIGUSR1));
  }
}

#ifndef __ANDROID__
static void cancelled_recipient(void *argument) {
  struct signal_slot *slot = argument;
  assert(!signal_prepare_release(slot, 0));
  signal_release(slot);
}

static void *cancel_after_read(void *unused) {
  (void)unused;
  struct exchange exchange = {.host_tid = syscall(SYS_gettid)};
  struct signal_slot *slot = signal_acquire(&exchange);
  assert(slot);
  recipient = slot;
  pthread_cleanup_push(cancelled_recipient, slot);
  queue(8100);
  inject = 2;
  struct signalfd_siginfo record;
  read(descriptor, &record, sizeof record);
  assert(!"completed read survived requested cancellation");
  pthread_cleanup_pop(1);
  return NULL;
}

static void cancelled_copy(void) {
  struct signal_slot *parent = recipient;
  pthread_t thread;
  assert(!pthread_create(&thread, NULL, cancel_after_read, NULL));
  void *returned;
  assert(!pthread_join(thread, &returned));
  assert(returned == PTHREAD_CANCELED);
  recipient = parent;
  drained();
}
#endif

static void drained(void) {
  assert(!signal_host_retirement_busy());
  sigset_t pending;
  assert(!sigpending(&pending) && !sigismember(&pending, SIGTRAP));
  struct signalfd_siginfo information;
  assert(read(descriptor, &information, sizeof information) == -1);
  assert(errno == EAGAIN);
}

static void returned_read(int vector) {
  const int modes[] = {1, 3, 5};
  for (unsigned i = 0; i < sizeof modes / sizeof modes[0]; ++i) {
    action = modes[i];
    queue(8100);
    inject = 1;
    struct signalfd_siginfo information;
    errno = EDOM;
    if (vector) {
      struct iovec part = {&information, sizeof information};
      assert(readv(descriptor, &part, 1) == sizeof information);
    } else {
      assert(read(descriptor, &information, sizeof information) ==
             sizeof information);
    }
    assert(errno == EDOM);
    verify(&information, 8100);
    assert(read(descriptor, &information, sizeof information) ==
           sizeof information);
    verify(&information, action == 3 ? 8300 : 8200);
    drained();
  }
}

static void ignored_consumption(void) {
  for (int wait = 0; wait < 2; ++wait) {
    queue(8100);
    inject = 3;
    if (wait) {
      siginfo_t information;
      assert(sigwaitinfo(&wanted, &information) == SIGTRAP);
      assert(information.si_value.sival_int == 8100);
    } else {
      struct signalfd_siginfo information;
      assert(read(descriptor, &information, sizeof information) ==
             sizeof information);
      verify(&information, 8100);
    }
    drained();
  }
}

static void jumped_read(int vector) {
  action = 2;
  queue(8100);
  int value = sigsetjmp(outside, 1);
  if (!value) {
    inject = 1;
    struct signalfd_siginfo information;
    struct iovec part = {&information, sizeof information};
    if (vector) {
      readv(descriptor, &part, 1);
    } else {
      read(descriptor, &information, sizeof information);
    }
    assert(!"native consumer returned after an outside jump");
  }
  assert(value == 19 && !gw_nonlocal_pending());
  struct signalfd_siginfo information;
  assert(read(descriptor, &information, sizeof information) ==
         sizeof information);
  verify(&information, 8200);
  drained();
}

static void consuming_waits(void) {
  for (int kind = 0; kind < 3; ++kind) {
    action = 1;
    queue(8100);
    inject = 1;
    siginfo_t information;
    errno = EDOM;
    if (kind == 0) {
      assert(sigwaitinfo(&wanted, &information) == SIGTRAP);
    } else if (kind == 1) {
      struct timespec timeout = {.tv_sec = 1};
      assert(sigtimedwait(&wanted, &information, &timeout) == SIGTRAP);
    } else {
      int number;
      assert(!sigwait(&wanted, &number) && number == SIGTRAP);
    }
    assert(errno == EDOM);
    if (kind != 2) {
      assert(information.si_value.sival_int == 8100);
    }
    struct signalfd_siginfo next;
    assert(read(descriptor, &next, sizeof next) == sizeof next);
    verify(&next, 8200);
    drained();
  }
  action = 4;
  queue(8100);
  int value = sigsetjmp(outside, 1);
  if (!value) {
    inject = 1;
    sigwaitinfo(&wanted, NULL);
    assert(!"wait returned after a nonlocal exit");
  }
  assert(value == 23 && !gw_nonlocal_pending());
  drained();
}

static void faulted_copy(void) {
  void *inaccessible =
      mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(inaccessible != MAP_FAILED);
  /* Establish the running kernel's native consumption on EFAULT. */
  uint64_t bits = UINT64_C(1) << (SIGTRAP - 1);
  int native = syscall(SYS_signalfd4, -1, &bits, 8, SFD_NONBLOCK | SFD_CLOEXEC);
  assert(native >= 0);
  assert(!syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), SIGTRAP));
  assert(syscall(SYS_read, native, inaccessible, 128) == -1 && errno == EFAULT);
  uint64_t pending;
  assert(!syscall(SYS_rt_sigpending, &pending, sizeof pending));
  assert(!(pending & bits));
  assert(!syscall(SYS_close, native));
  queue(8100);
  assert(read(descriptor, inaccessible, 128) == -1 && errno == EFAULT);
  drained();
  assert(!munmap(inaccessible, 4096));
  /* A valid zero-iovec read returns zero rather than retrying forever. */
  struct iovec empty = {0};
  assert(readv(descriptor, &empty, 0) == 0);
  queue(8100);
  assert(readv(descriptor, &empty, 0) == 0);
  unsigned char zero;
  assert(read(descriptor, &zero, 0) == -1 && errno == EINVAL);
  struct signalfd_siginfo record;
  assert(read(descriptor, &record, 127) == -1 && errno == EINVAL);
  assert(read(descriptor, &record, sizeof record) == sizeof record);
  verify(&record, 8100);
  drained();
}

int main(void) {
  assert(!gw_nonlocal_init() && gw_nonlocal_ready());
  assert(!signal_router_init());
  const int notifications[] = {SIGRTMIN,     SIGRTMIN + 1, SIGRTMIN + 2,
                               SIGRTMIN + 3, SIGRTMIN + 4, SIGRTMIN + 5};
  assert(!signal_router_defer_faults(notifications));
  struct sigaction handler = {.sa_handler = receive};
  sigemptyset(&handler.sa_mask);
  assert(!sigaction(SIGUSR1, &handler, NULL));
  sigemptyset(&wanted);
  sigaddset(&wanted, SIGTRAP);
  sigset_t previous;
  assert(!pthread_sigmask(SIG_BLOCK, &wanted, &previous));
  struct exchange exchange = {.host_tid = syscall(SYS_gettid)};
  recipient = signal_acquire(&exchange);
  assert(recipient);
  descriptor = signalfd(-1, &wanted, SFD_NONBLOCK | SFD_CLOEXEC);
  assert(descriptor >= 0);
  returned_read(0);
  returned_read(1);
  ignored_consumption();
  jumped_read(0);
  jumped_read(1);
  consuming_waits();
  faulted_copy();
#ifndef __ANDROID__
  cancelled_copy();
#endif
  assert(deliveries == 12);
  assert(!signal_prepare_release(recipient, 0));
  signal_release(recipient);
  assert(!close(descriptor));
  assert(!signal_router_restore());
  assert(!pthread_sigmask(SIG_SETMASK, &previous, NULL));
  puts(
      "{\"result\":\"PASS\",\"read_unwind\":true,\"readv_unwind\":true,"
      "\"wait_unwind\":true,\"handler_fresh_signal\":true,"
      "\"nested_reader\":true,\"handler_local_jump\":true,"
      "\"native_efault_consumption\":true,\"zero_iovec\":true,"
      "\"scalar_zero_einval\":true,"
      "\"committed_before_handler\":true,\"retirement_retry\":true,"
      "\"no_stack_consumer\":true,"
      "\"ignore_after_consumption\":true,"
#ifdef __ANDROID__
      "\"pthread_cancel_unavailable\":true}"
#else
      "\"cancel_after_consumption\":true}"
#endif
  );
}
