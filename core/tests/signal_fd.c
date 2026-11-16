#define _GNU_SOURCE
#include "signals.h"
#include "signal-router.h"
#include "nonlocal.h"
#include "nonlocal-mask.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/prctl.h>
#include <sys/select.h>
#include <sys/signalfd.h>
#include <sys/uio.h>
#include <sys/wait.h>

static sigset_t wanted;
static int split_records;
static _Atomic long waiting_tid;
static volatile sig_atomic_t interrupted_wait;
static int reuse_on_close = -1;
static int escape_on_close = -1, replacement = -1;
static volatile sig_atomic_t close_mode;
static int native_close_window, native_close_observations;
static sigjmp_buf close_target;
extern int signal_fd_channel_busy(void);

static void escape_close(int number) {
  assert(number == SIGUSR2);
  siglongjmp(close_target, 31);
}

static void inaccessible_proc(int descriptor) {
  pid_t child = fork();
  assert(child >= 0);
  if (!child) {
    struct sock_filter filter[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_readlinkat, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)};
    struct sock_fprog program = {sizeof filter / sizeof filter[0], filter};
    assert(!prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0));
    assert(!prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program));
    struct signalfd_siginfo information;
    assert(read(descriptor, &information, sizeof information) == -1);
    assert(errno == ENOTSUP);
    _exit(0);
  }
  int status;
  assert(waitpid(child, &status, 0) == child);
  assert(WIFEXITED(status) && !WEXITSTATUS(status));
}

void signal_fd_close_observe(int descriptor) {
  if (descriptor == escape_on_close) {
    escape_on_close = -1;
    if (close_mode & 2) {
      replacement = signalfd(-1, &wanted, SFD_NONBLOCK | SFD_CLOEXEC);
      assert(replacement == descriptor);
    }
#ifndef __ANDROID__
    if (close_mode & 1) {
      assert(!pthread_cancel(pthread_self()));
      pthread_testcancel();
      assert(!"cancelled close returned");
    }
#endif
    assert(!raise(SIGUSR2));
    assert(!"close returned after an outside signal jump");
  }
  if (descriptor != reuse_on_close) {
    return;
  }
  reuse_on_close = -1;
  int pipefd[2];
  assert(!pipe2(pipefd, O_CLOEXEC | O_NONBLOCK));
  assert(pipefd[0] == descriptor);
  struct signalfd_siginfo bytes = {.ssi_signo = SIGRTMIN}, observed;
  assert(write(pipefd[1], &bytes, sizeof bytes) == sizeof bytes);
  /* Kernel close completed, but its wrapper has not retired old metadata.
   * Data from this unrelated new pipe must never be decoded as signalfd. */
  assert(read(pipefd[0], &observed, sizeof observed) == sizeof observed);
  assert(!memcmp(&observed, &bytes, sizeof bytes));
  assert(!close(pipefd[0]) && !close(pipefd[1]));
}

void signal_fd_native_close_observe(int descriptor) {
  if (native_close_window && descriptor == escape_on_close) {
    ++native_close_observations;
    signal_fd_close_observe(descriptor);
  }
}

static void jumped_close(int reuse) {
  int descriptor = signalfd(-1, &wanted, SFD_NONBLOCK | SFD_CLOEXEC);
  assert(descriptor >= 0 && signal_fd_channel_busy());
  close_mode = reuse ? 2 : 0;
  escape_on_close = descriptor;
  int result = sigsetjmp(close_target, 1);
  if (!result) {
    close(descriptor);
    assert(!"managed close did not jump");
  }
  assert(result == 31 && !gw_nonlocal_pending());
  if (reuse) {
    assert(replacement == descriptor && signal_fd_channel_busy());
    assert(fcntl(replacement, F_GETFD) >= 0);
    assert(!close(replacement));
  } else {
    assert(fcntl(descriptor, F_GETFD) == -1 && errno == EBADF);
  }
  assert(!signal_fd_channel_busy());
}

#ifndef __ANDROID__
static void *cancelled_close(void *argument) {
  int descriptor = *(int *)argument;
  if (close_mode == 4) {
    /* Pending cancellation must still be tested by native close. Cleanup
     * cannot assume the syscall ran and erase a descriptor still open. */
    assert(!pthread_cancel(pthread_self()));
  }
  close(descriptor);
  assert(!"managed close suppressed native cancellation");
  return NULL;
}

static void close_cancellation(int mode) {
  int descriptor = signalfd(-1, &wanted, SFD_NONBLOCK | SFD_CLOEXEC);
  assert(descriptor >= 0);
  close_mode = mode;
  escape_on_close = mode == 4 ? -1 : descriptor;
  pthread_t thread;
  assert(!pthread_create(&thread, NULL, cancelled_close, &descriptor));
  void *returned;
  assert(!pthread_join(thread, &returned));
  assert(returned == PTHREAD_CANCELED && !gw_nonlocal_pending());
  int still_open = fcntl(descriptor, F_GETFD) >= 0;
  if (mode == 3) {
    assert(still_open && replacement == descriptor);
  } else if (mode == 1) {
    assert(!still_open && errno == EBADF);
  }
  /* Native cancellation before close is allowed to leave the descriptor
   * open. Its metadata must agree with the observed kernel result. */
  assert(!!signal_fd_channel_busy() == still_open);
  if (still_open) {
    assert(!close(descriptor));
  }
  assert(!signal_fd_channel_busy());
}
#endif

static void close_unwinds(void) {
  assert(!signal_fd_channel_busy());
  struct sigaction action = {.sa_handler = escape_close}, previous;
  sigemptyset(&action.sa_mask);
  assert(!sigaction(SIGUSR2, &action, &previous));
  jumped_close(0);
  jumped_close(1);
#ifndef __ANDROID__
  close_cancellation(1);
  close_cancellation(3);
  close_cancellation(4);
#endif
  assert(!sigaction(SIGUSR2, &previous, NULL));
}

static void interrupt_reader(int number) {
  assert(number == SIGUSR1);
  interrupted_wait = 1;
}

static void cleanup_reader(void *argument) {
  struct signal_slot *slot = argument;
  assert(!signal_prepare_release(slot, 1));
  signal_release(slot);
}

struct blocking_reader {
  int descriptor, interrupt, use_vector;
};

static void *blocking_reader(void *argument) {
  const struct blocking_reader *reader = argument;
  struct exchange exchange = {0};
  exchange.host_tid = syscall(SYS_gettid);
  struct signal_slot *slot = signal_acquire(&exchange);
  assert(slot);
  sigset_t blocked;
  sigemptyset(&blocked);
  sigaddset(&blocked, SIGTRAP);
  assert(!pthread_sigmask(SIG_BLOCK, &blocked, NULL));
  sigemptyset(&blocked);
  sigaddset(&blocked, SIGUSR1);
  assert(!pthread_sigmask(SIG_UNBLOCK, &blocked, NULL));
  pthread_cleanup_push(cleanup_reader, slot);
  atomic_store_explicit(&waiting_tid, exchange.host_tid, memory_order_release);
  struct signalfd_siginfo record;
  struct iovec vector = {&record, sizeof record};
  ssize_t result = reader->use_vector
                       ? readv(reader->descriptor, &vector, 1)
                       : read(reader->descriptor, &record, sizeof record);
  assert(reader->interrupt && result == -1 && errno == EINTR);
  assert(interrupted_wait);
  pthread_cleanup_pop(1);
  return NULL;
}

static void wait_until_blocked(void) {
  /* Observe the real syscall, not a delay or a "ready" flag preceding it.
   * /proc/self/task is accessible in both tested native shell environments. */
  struct timespec started, now;
  assert(!clock_gettime(CLOCK_MONOTONIC, &started));
  for (;;) {
    long tid = atomic_load_explicit(&waiting_tid, memory_order_acquire);
    if (tid) {
      char path[96], text[256];
      snprintf(path, sizeof path, "/proc/self/task/%ld/syscall", tid);
      int fd = open(path, O_RDONLY | O_CLOEXEC);
      assert(fd >= 0);
      ssize_t length = read(fd, text, sizeof text - 1);
      assert(!close(fd));
      if (length > 0) {
        text[length] = 0;
        long number;
        if (sscanf(text, "%ld", &number) == 1 &&
            (number == SYS_read || number == SYS_readv)) {
          return;
        }
      }
    }
    assert(!clock_gettime(CLOCK_MONOTONIC, &now));
    assert(now.tv_sec - started.tv_sec < 5);
    sched_yield();
  }
}

static void blocking_contract(void) {
  struct sigaction action = {.sa_handler = interrupt_reader}, previous;
  sigemptyset(&action.sa_mask);
  assert(!sigaction(SIGUSR1, &action, &previous));
#ifdef __ANDROID__
  const int first_interrupt = 1; /* Bionic exposes no pthread_cancel API. */
#else
  const int first_interrupt = 0;
#endif
  for (int native = 0; native < 2; ++native) {
    for (int vector = 0; vector < 2; ++vector) {
      for (int interrupt = first_interrupt; interrupt < 2; ++interrupt) {
        uint64_t bits = UINT64_C(1) << (SIGTRAP - 1);
        int descriptor = native
                             ? syscall(SYS_signalfd4, -1, &bits, 8, SFD_CLOEXEC)
                             : signalfd(-1, &wanted, SFD_CLOEXEC);
        assert(descriptor >= 0);
        struct blocking_reader request = {descriptor, interrupt, vector};
        pthread_t thread;
        atomic_store_explicit(&waiting_tid, 0, memory_order_release);
        interrupted_wait = 0;
        assert(!pthread_create(&thread, NULL, blocking_reader, &request));
        wait_until_blocked();
        if (interrupt) {
          assert(!pthread_kill(thread, SIGUSR1));
        } else {
#ifndef __ANDROID__
          assert(!pthread_cancel(thread));
#else
          assert(0);
#endif
        }
        void *returned;
        assert(!pthread_join(thread, &returned));
        void *expected = NULL;
#ifndef __ANDROID__
        if (!interrupt) {
          expected = PTHREAD_CANCELED;
        }
#endif
        assert(returned == expected);
        assert(!close(descriptor));
      }
    }
  }
  assert(!sigaction(SIGUSR1, &previous, NULL));
}

static int native_split_readv(void) {
  sigset_t blocked, previous;
  sigemptyset(&blocked);
  sigaddset(&blocked, SIGUSR1);
  assert(!pthread_sigmask(SIG_BLOCK, &blocked, &previous));
  uint64_t bits = UINT64_C(1) << (SIGUSR1 - 1);
  int descriptor =
      syscall(SYS_signalfd4, -1, &bits, 8, SFD_NONBLOCK | SFD_CLOEXEC);
  assert(descriptor >= 0);
  assert(!syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), SIGUSR1));
  unsigned char bytes[256] = {0};
  struct iovec vectors[] = {
      {bytes, 0}, {bytes, 7}, {bytes + 7, 121}, {bytes + 128, 128}};
  ssize_t result = syscall(SYS_readv, descriptor, vectors, 4);
  int split = result == 128;
  if (!split) {
    /* Legacy kernels loop over .read per iovec; a seven-byte first read is
     * EINVAL and consumes nothing. New kernels use one .read_iter transaction.
     * https://github.com/torvalds/linux/blob/v5.4/fs/signalfd.c
     * https://github.com/torvalds/linux/blob/v5.4/fs/read_write.c
     */
    assert(result == -1 && errno == EINVAL);
    assert(syscall(SYS_read, descriptor, bytes, sizeof bytes) == 128);
  }
  assert(!syscall(SYS_close, descriptor));
  assert(!pthread_sigmask(SIG_SETMASK, &previous, NULL));
  return split;
}

static void queue(struct signal_slot *slot, int code, int value) {
  siginfo_t information = {.si_signo = SIGTRAP, .si_code = code};
  information.si_pid = getpid();
  information.si_uid = getuid();
  information.si_value.sival_int = value;
  uint64_t previous = gw_nonlocal_mask_block();
  assert(!gw_signal_deferred_put(slot->deferred, &information));
  gw_nonlocal_mask_restore(previous);
}

static void record(const struct signalfd_siginfo *information, int value) {
  assert(information->ssi_signo == SIGTRAP);
  assert(information->ssi_code == SI_QUEUE);
  assert(information->ssi_pid == (unsigned)getpid());
  assert(information->ssi_uid == getuid());
  assert(information->ssi_int == value);
}

static struct signal_slot *acquire(struct exchange *exchange) {
  exchange->host_tid = syscall(SYS_gettid);
  struct signal_slot *slot = signal_acquire(exchange);
  assert(slot && slot->deferred_active);
  assert(!pthread_sigmask(SIG_BLOCK, &wanted, NULL));
  return slot;
}

static void readiness(int descriptor, int ready) {
  struct pollfd pollfd = {.fd = descriptor, .events = POLLIN};
  assert(poll(&pollfd, 1, 0) == ready);
  assert((!!(pollfd.revents & POLLIN)) == ready);
  struct timespec zero = {0};
  assert(ppoll(&pollfd, 1, &zero, NULL) == ready);
  struct timeval timeout = {0};
  fd_set read;
  FD_ZERO(&read);
  FD_SET(descriptor, &read);
  assert(select(descriptor + 1, &read, NULL, NULL, &timeout) == ready);
  FD_ZERO(&read);
  FD_SET(descriptor, &read);
  assert(pselect(descriptor + 1, &read, NULL, NULL, &zero, NULL) == ready);
  int epoll = epoll_create1(EPOLL_CLOEXEC);
  assert(epoll >= 0);
  struct epoll_event event = {.events = EPOLLIN, .data.u64 = 7193};
  assert(!epoll_ctl(epoll, EPOLL_CTL_ADD, descriptor, &event));
  assert(epoll_wait(epoll, &event, 1, 0) == ready);
  assert(epoll_pwait(epoll, &event, 1, 0, NULL) == ready);
  assert(!close(epoll));
}

static pthread_barrier_t ignore_barrier;
static volatile sig_atomic_t ignored_deliveries;

static void ignored_delivery(int number) {
  assert(number == SIGTRAP);
  ++ignored_deliveries;
}

static void *replace_ignored_action(void *unused) {
  (void)unused;
  struct sigaction ignore = {.sa_handler = SIG_IGN}, previous;
  sigemptyset(&ignore.sa_mask);
  assert(!sigaction(SIGTRAP, NULL, &previous));
  for (int round = 0; round < 64; ++round) {
    int result = pthread_barrier_wait(&ignore_barrier);
    assert(!result || result == PTHREAD_BARRIER_SERIAL_THREAD);
    assert(!sigaction(SIGTRAP, &ignore, NULL));
    assert(!sigaction(SIGTRAP, &previous, NULL));
    result = pthread_barrier_wait(&ignore_barrier);
    assert(!result || result == PTHREAD_BARRIER_SERIAL_THREAD);
  }
  return NULL;
}

static void ignored_pending(int descriptor, struct signal_slot *slot) {
  struct sigaction receiver = {.sa_handler = ignored_delivery}, inherited;
  sigemptyset(&receiver.sa_mask);
  assert(!sigaction(SIGTRAP, &receiver, &inherited));
  assert(!pthread_barrier_init(&ignore_barrier, NULL, 2));
  pthread_t thread;
  assert(!pthread_create(&thread, NULL, replace_ignored_action, NULL));
  siginfo_t source = {.si_signo = SIGTRAP, .si_code = SI_QUEUE};
  source.si_pid = getpid();
  source.si_uid = getuid();
  for (int round = 0; round < 64; ++round) {
    source.si_value.sival_int = 8400;
    if (round & 1) {
      assert(!syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), SIGTRAP));
    } else {
      assert(!signal_host_defer(slot, &source));
    }
    int result = pthread_barrier_wait(&ignore_barrier);
    assert(!result || result == PTHREAD_BARRIER_SERIAL_THREAD);
    result = pthread_barrier_wait(&ignore_barrier);
    assert(!result || result == PTHREAD_BARRIER_SERIAL_THREAD);
    /* The recipient executes no adapter between ignore and replacement. Its
     * old pending signal must still disappear, despite the restored handler. */
    if (round & 2) {
      /* No query/decoder runs before unmasking: invalidated levels must not
       * reach the restored action as if they were fresh signals. */
      assert(!pthread_sigmask(SIG_UNBLOCK, &wanted, NULL));
      assert(!ignored_deliveries);
      assert(!pthread_sigmask(SIG_BLOCK, &wanted, NULL));
    }
    sigset_t pending;
    assert(!sigpending(&pending) && !sigismember(&pending, SIGTRAP));
    readiness(descriptor, 0);
    struct signalfd_siginfo record;
    assert(read(descriptor, &record, sizeof record) == -1 && errno == EAGAIN);
    source.si_value.sival_int = 8500 + round;
    assert(!signal_host_defer(slot, &source));
    assert(read(descriptor, &record, sizeof record) == sizeof record);
    assert(record.ssi_int == 8500 + round);
  }
  assert(!pthread_join(thread, NULL));
  assert(!pthread_barrier_destroy(&ignore_barrier));
  struct sigaction ignore = {.sa_handler = SIG_IGN}, previous;
  sigemptyset(&ignore.sa_mask);
  assert(!sigaction(SIGTRAP, &ignore, &previous));
  assert(!signal_host_defer(slot, &source));
  assert(!sigaction(SIGTRAP, &previous, NULL));
  sigset_t pending;
  assert(!sigpending(&pending) && !sigismember(&pending, SIGTRAP));
  readiness(descriptor, 0);
  /* A libc jump restores its saved mask through a private entry. */
  sigjmp_buf target;
  assert(!pthread_sigmask(SIG_UNBLOCK, &wanted, NULL));
  int value = sigsetjmp(target, 1);
  if (!value) {
    assert(!pthread_sigmask(SIG_BLOCK, &wanted, NULL));
    assert(!signal_host_defer(slot, &source));
    assert(!sigaction(SIGTRAP, &ignore, &previous));
    assert(!sigaction(SIGTRAP, &previous, NULL));
    siglongjmp(target, 41);
  }
  assert(value == 41 && !ignored_deliveries);
  assert(!pthread_sigmask(SIG_BLOCK, &wanted, NULL));
  readiness(descriptor, 0);
  assert(!sigaction(SIGTRAP, &inherited, NULL));
}

static void records_and_aliases(int descriptor, struct signal_slot *slot) {
  queue(slot, SI_QUEUE, 7193);
  queue(slot, SI_QUEUE, 9999);
  readiness(descriptor, 1);
  struct signalfd_siginfo information;
  errno = EDOM;
  assert(read(descriptor, &information, sizeof information) == 128);
  assert(errno == EDOM);
  record(&information, 7193);
  readiness(descriptor, 0);
  assert(read(descriptor, &information, sizeof information) == -1);
  assert(errno == EAGAIN);
  int aliases[] = {dup(descriptor), fcntl(descriptor, F_DUPFD, 0),
                   fcntl(descriptor, F_DUPFD_CLOEXEC, 0)};
  sigset_t empty;
  sigemptyset(&empty);
  for (unsigned i = 0; i < sizeof aliases / sizeof aliases[0]; ++i) {
    int fd = aliases[i];
    assert(fd >= 0);
    assert(fcntl(fd, F_GETFD) >= 0);
    assert(signalfd(fd, &empty, 0) == fd);
    queue(slot, SI_QUEUE, 7200 + i);
    readiness(descriptor, 0); /* A mask update through an alias changes OFD. */
    assert(signalfd(fd, &wanted, 0) == fd);
    readiness(descriptor, 1);
    unsigned char bytes[256] = {0};
    struct iovec vectors[] = {
        {bytes, 0}, {bytes, 7}, {bytes + 7, 121}, {bytes + 128, 128}};
    ssize_t size = readv(fd, vectors, 4);
    if (!split_records) {
      assert(size == -1 && errno == EINVAL);
      struct iovec aligned[] = {{bytes, 128}, {bytes + 128, 128}};
      size = readv(fd, aligned, 2);
    }
    assert(size == 128);
    memcpy(&information, bytes, sizeof information);
    record(&information, 7200 + i);
    assert(!close(fd));
  }
  queue(slot, SI_QUEUE, 7300);
  uint64_t previous = gw_nonlocal_mask_block();
  gw_signal_deferred_discard(slot->deferred, SIGTRAP);
  gw_nonlocal_mask_restore(previous);
  readiness(descriptor, 0); /* Cancelled levels are never readable EOF/data. */
  assert(!signal_host_retirement_busy());
  assert(read(descriptor, &information, sizeof information) == -1);
  assert(errno == EAGAIN);
}

static void mixed_and_replacement(int descriptor, struct signal_slot *slot) {
  sigset_t mixed = wanted;
  sigaddset(&mixed, SIGUSR1);
  assert(!pthread_sigmask(SIG_BLOCK, &mixed, NULL));
  assert(signalfd(descriptor, &mixed, 0) == descriptor);
  queue(slot, SI_QUEUE, 7400);
  assert(!syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), SIGUSR1));
  struct signalfd_siginfo information[2];
  assert(read(descriptor, information, sizeof information) == 256);
  int ordinary = 0, deferred = 0;
  for (int i = 0; i < 2; ++i) {
    if (information[i].ssi_signo == SIGUSR1) {
      assert(information[i].ssi_code == SI_TKILL);
      ++ordinary;
    } else {
      record(&information[i], 7400);
      ++deferred;
    }
  }
  assert(ordinary == 1 && deferred == 1);
  int pipefd[2];
  assert(!pipe(pipefd));
  int target = dup(pipefd[0]);
  assert(target >= 0 && dup2(descriptor, target) == target);
  queue(slot, SI_QUEUE, 7500);
  assert(read(target, information, sizeof information) == 128);
  record(information, 7500);
  assert(dup3(pipefd[0], target, O_CLOEXEC) == target);
  unsigned char bytes[128] = {0}, observed[128];
  /* Ordinary data starts with a private number: stale metadata would eat it. */
  bytes[0] = SIGRTMIN;
  assert(write(pipefd[1], bytes, sizeof bytes) == sizeof bytes);
  assert(read(target, observed, sizeof observed) == sizeof observed);
  assert(!memcmp(bytes, observed, sizeof bytes));
  assert(!close(target) && !close(pipefd[0]) && !close(pipefd[1]));
  assert(signalfd(descriptor, &wanted, 0) == descriptor);
  assert(read(descriptor, information, 127) == -1 && errno == EINVAL);
}

static void *reader_thread(void *argument) {
  int descriptor = *(int *)argument;
  struct exchange exchange = {0};
  struct signal_slot *slot = acquire(&exchange);
  queue(slot, SI_QUEUE, 7600);
  struct signalfd_siginfo information;
  assert(read(descriptor, &information, sizeof information) == 128);
  record(&information, 7600);
  assert(!signal_prepare_release(slot, 0));
  signal_release(slot);
  return NULL;
}

static void native_metadata(int descriptor, struct signal_slot *slot) {
  /* Compare the entire kernel record, including zero padding and si_value's
   * pointer/int overlap. This catches copying the wrong libc union member. */
  const int codes[] = {SI_QUEUE, SI_USER, SI_TKILL, SI_TIMER, SI_SIGIO};
  for (unsigned i = 0; i < sizeof codes / sizeof codes[0]; ++i) {
    siginfo_t source = {.si_signo = SIGTRAP, .si_code = codes[i]};
    source.si_pid = getpid();
    source.si_uid = getuid();
    source.si_value.sival_ptr = (void *)(uintptr_t)0x123456787193ULL;
    if (codes[i] == SI_TIMER) {
      source.si_timerid = 7;
      source.si_overrun = 9;
    } else if (codes[i] == SI_SIGIO) {
      source.si_band = 13;
      source.si_fd = 17;
    }
    uint64_t bits = UINT64_C(1) << (SIGTRAP - 1);
    int native =
        syscall(SYS_signalfd4, -1, &bits, 8, SFD_NONBLOCK | SFD_CLOEXEC);
    assert(native >= 0);
    /* Linux refuses forged SI_TKILL queue sends; tgkill supplies its metadata.
     */
    if (codes[i] == SI_TKILL) {
      assert(!syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), SIGTRAP));
    } else {
      assert(!syscall(SYS_rt_tgsigqueueinfo, getpid(), syscall(SYS_gettid),
                      SIGTRAP, &source));
    }
    struct signalfd_siginfo expected, actual;
    assert(syscall(SYS_read, native, &expected, sizeof expected) == 128);
    assert(!syscall(SYS_close, native));
    uint64_t previous = gw_nonlocal_mask_block();
    assert(!gw_signal_deferred_put(slot->deferred, &source));
    gw_nonlocal_mask_restore(previous);
    assert(read(descriptor, &actual, sizeof actual) == 128);
    if (codes[i] == SI_TKILL) {
      expected.ssi_pid = source.si_pid;
      expected.ssi_uid = source.si_uid;
    }
    assert(!memcmp(&expected, &actual, sizeof actual));
  }
}

int main(int argc, char **argv) {
  assert(argc == 1 || (argc == 2 && !strcmp(argv[1], "--native-close-window")));
  native_close_window = argc == 2;
  assert(!gw_nonlocal_init() && gw_nonlocal_ready());
  sigemptyset(&wanted);
  sigaddset(&wanted, SIGTRAP);
  sigset_t inherited;
  assert(!pthread_sigmask(SIG_SETMASK, NULL, &inherited));
  split_records = native_split_readv();
  assert(!signal_router_init());
  const int notifications[] = {SIGRTMIN,     SIGRTMIN + 1, SIGRTMIN + 2,
                               SIGRTMIN + 3, SIGRTMIN + 4, SIGRTMIN + 5};
  int before = signalfd(-1, &wanted, SFD_NONBLOCK | SFD_CLOEXEC);
  assert(before >= 0);
  assert(signal_router_defer_faults(notifications) == EBUSY);
  assert(!close(before));
  assert(!signal_router_defer_faults(notifications));
  /* Create before carrier registration; consumption belongs to the reader. */
  int descriptor = signalfd(-1, &wanted, SFD_NONBLOCK | SFD_CLOEXEC);
  assert(descriptor >= 0);
  struct exchange exchange = {0};
  struct signal_slot *slot = acquire(&exchange);
  records_and_aliases(descriptor, slot);
  mixed_and_replacement(descriptor, slot);
  native_metadata(descriptor, slot);
  ignored_pending(descriptor, slot);
  pthread_t reader;
  assert(!pthread_create(&reader, NULL, reader_thread, &descriptor));
  assert(!pthread_join(reader, NULL));
  blocking_contract();
  inaccessible_proc(descriptor);
  assert(!signal_prepare_release(slot, 0));
  signal_release(slot);
  assert(signal_router_restore() == EBUSY); /* Live mapped OFD retains lanes. */
  assert(!close(descriptor));
  int reused = signalfd(-1, &wanted, SFD_NONBLOCK | SFD_CLOEXEC);
  assert(reused >= 0);
  reuse_on_close = reused;
  assert(!close(reused) && reuse_on_close == -1);
  close_unwinds();
  if (native_close_window) {
#ifdef __ANDROID__
    assert(native_close_observations == 2);
#else
    assert(native_close_observations == 4);
#endif
  }
  assert(!signal_router_restore());
  assert(!pthread_sigmask(SIG_SETMASK, &inherited, NULL));
  printf("{\"result\":\"PASS\",%s",
         native_close_window ? "\"native_close_window\":true," : "");
  puts(
      "\"metadata_native_comparison\":true,"
      "\"readv_native_layout\":true,\"readiness\":true,\"cancelled_level\":"
      "true,"
      "\"duplicate_mask_update\":true,\"fd_replacement\":true,"
      "\"per_reader_recipient\":true,\"channel_lifetime\":true,"
      "\"close_fd_reuse_window\":true,"
      "\"close_jump_cleanup\":true,\"close_reuse_generation\":true,"
      "\"concurrent_ignore_generation\":true,"
      "\"ignore_before_unmask\":true,\"ignore_before_jump\":true,"
      "\"inaccessible_proc_rejected\":true,"
      "\"native_eintr_comparison\":true,"
#ifdef __ANDROID__
      "\"pthread_cancel_unavailable\":true}"
#else
      "\"native_cancel_comparison\":true,\"close_cancel_cleanup\":true}"
#endif
  );
}
