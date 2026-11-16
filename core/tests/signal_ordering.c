#define _GNU_SOURCE
#include "signals.h"
#include "signal-router.h"
#include "signal_channel.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <sched.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/uio.h>
#include <sys/wait.h>
#ifndef __ANDROID__
#include <ucontext.h>
#endif

static struct signal_slot *recipient;
static sigset_t wanted;
static int descriptor, race, hook, close_hook;
static FILE *stream;
static sigjmp_buf escape;
#ifndef __ANDROID__
static ucontext_t saved, suspended, caller;
static unsigned char continuation_stack[65536];
static int returned;
#endif

static siginfo_t packet(int number, int value) {
  siginfo_t info = {.si_signo = number, .si_code = SI_QUEUE};
  info.si_pid = getpid();
  info.si_uid = getuid();
  info.si_value.sival_int = value;
  return info;
}

static void send_native(int number, int value, int shared) {
  siginfo_t info = packet(number, value);
  int result = shared ? syscall(SYS_rt_sigqueueinfo, getpid(), number, &info)
                      : syscall(SYS_rt_tgsigqueueinfo, getpid(),
                                syscall(SYS_gettid), number, &info);
  assert(!result);
}

static void send_packet(int number, int value, int shared, int routed) {
  uint64_t bit = UINT64_C(1) << (number - 1);
  if (routed && !shared && (gw_signal_fault_bits() & bit)) {
    siginfo_t info = packet(number, value);
    assert(!signal_host_defer(recipient, &info));
  } else {
    send_native(number, value, shared);
  }
}

void gw_signal_selection_test_point(void) {
  if (race) {
    race = 0;
    send_native(SIGILL, 9911, 0);
  }
}

void gw_signal_consume_test_point(void) {
  int action = hook;
  hook = 0;
  if (action == 1) {
    siglongjmp(escape, 1);
  }
  if (action == 4) {
    send_native(SIGTRAP, 9999, 0);
  }
#ifndef __ANDROID__
  if (action == 2) {
    assert(!setcontext(&saved));
  }
  if (action == 3) {
    assert(!swapcontext(&suspended, &caller));
  }
#endif
}

void signal_fd_close_observe(int fd) {
  (void)fd;
  int action = close_hook;
  close_hook = 0;
  if (action == 1) {
    siglongjmp(escape, 1);
  }
#ifndef __ANDROID__
  if (action == 2) {
    assert(!setcontext(&saved));
  }
#endif
}

static int consume(int method, int routed, int *value) {
  if (!routed) {
    siginfo_t info;
    uint64_t bits;
    memcpy(&bits, &wanted, sizeof bits);
    struct timespec zero = {0};
    int number = syscall(SYS_rt_sigtimedwait, &bits, &info, &zero, 8);
    assert(number > 0);
    *value = info.si_value.sival_int;
    return number;
  }

  if (method < 3) {
    siginfo_t info;
    struct timespec zero = {0};
    int number = method == 0   ? sigtimedwait(&wanted, &info, &zero)
                 : method == 1 ? sigwaitinfo(&wanted, &info)
                               : 0;
    if (method == 2) {
      assert(!sigwait(&wanted, &number));
      *value = -1;
    } else {
      assert(number > 0 && info.si_pid == getpid() && info.si_uid == getuid() &&
             info.si_code == SI_QUEUE);
      *value = info.si_value.sival_int;
    }
    return number;
  }

  struct signalfd_siginfo record;
  if (method == 3) {
    assert(read(descriptor, &record, sizeof record) == sizeof record);
  } else if (method == 4) {
    struct iovec vector = {&record, sizeof record};
    assert(readv(descriptor, &vector, 1) == sizeof record);
  } else {
    assert(fread(&record, sizeof record, 1, stream) == 1);
  }
  assert(record.ssi_pid == (unsigned)getpid() && record.ssi_uid == getuid() &&
         record.ssi_code == SI_QUEUE);
  *value = record.ssi_int;
  return record.ssi_signo;
}

static void begin(int method) {
  sigfillset(&wanted);
  assert(!pthread_sigmask(SIG_BLOCK, &wanted, NULL));
  assert(!signal_router_init());
  const int notifications[] = {SIGRTMIN,     SIGRTMIN + 1, SIGRTMIN + 2,
                               SIGRTMIN + 3, SIGRTMIN + 4, SIGRTMIN + 5};
  assert(gw_host_signal_channel_enable(NULL, 6) == EINVAL);
  assert(gw_host_signal_channel_enable(notifications, 5) == EINVAL);
  assert(!gw_host_signal_channel_enable(notifications, 6));
  for (int i = 0; i < 6; ++i) {
    sigdelset(&wanted, notifications[i]);
  }

  struct exchange *exchange = calloc(1, sizeof *exchange);
  assert(exchange);
  exchange->host_tid = syscall(SYS_gettid);
  recipient = signal_acquire(exchange);
  assert(recipient);
  descriptor = signalfd(-1, &wanted, SFD_NONBLOCK | SFD_CLOEXEC);
  assert(descriptor >= 0);
  if (method == 5) {
    stream = fdopen(descriptor, "r");
    assert(stream && !setvbuf(stream, NULL, _IONBF, 0));
  }
}

static void finish(int method) {
  assert(!signal_host_retirement_busy());
  assert(!signal_prepare_release(recipient, 0));
  struct exchange *exchange = recipient->exchange;
  signal_release(recipient);
  free(exchange);
  assert(method == 5 ? !fclose(stream) : !close(descriptor));
  assert(!signal_router_restore());
}

static void order_case(int method, int variant) {
  int observed[2][16], values[2][16];
  const int mixed[] = {SIGSEGV, SIGHUP, SIGSYS, SIGUSR1,
                       SIGILL,  SIGBUS, SIGFPE, SIGTRAP};
  for (int routed = 0; routed < 2; ++routed) {
    int count = 0;
    if (routed) {
      begin(method);
    } else {
      sigfillset(&wanted);
      assert(!pthread_sigmask(SIG_BLOCK, &wanted, NULL));
    }

    if (variant == 0) {
      for (unsigned i = 0; i < sizeof mixed / sizeof mixed[0]; ++i) {
        send_packet(mixed[i], 9000 + mixed[i], 0, routed);
        ++count;
      }
      for (int i = 0; i < 3; ++i) {
        send_native(SIGRTMIN + 12, 9500 + i, 0);
        ++count;
      }
    } else if (variant == 1) {
      send_packet(SIGSEGV, 9000 + SIGSEGV, 0, routed);
      send_packet(SIGUSR1, 9000 + SIGUSR1, 0, routed);
      send_packet(SIGILL, 9000 + SIGILL, 1, routed);
      count = 3;
    } else if (variant == 2) {
      send_packet(SIGTRAP, 9001, 0, routed);
      send_packet(SIGTRAP, 9002, 0, routed);
      send_native(SIGTRAP, 9003, 0);
      count = 1;
    } else {
      send_packet(SIGTRAP, 9001, 0, routed);
      send_native(SIGTRAP, 9002, 1);
      count = 2;
    }

    for (int i = 0; i < count; ++i) {
      observed[routed][i] = consume(method, routed, &values[routed][i]);
      if (routed) {
        assert(observed[0][i] == observed[1][i]);
        assert(method == 2 || values[0][i] == values[1][i]);
      }
    }
    uint64_t bits;
    assert(!syscall(SYS_rt_sigpending, &bits, 8) && !bits);
    if (routed) {
      finish(method);
    }
  }
}

static void concurrent_arrival(int method) {
  begin(method);
  send_packet(SIGSEGV, 9000, 0, 1);
  race = 1;
  int value;
  assert(consume(method, 1, &value) == SIGILL);
  assert(method == 2 || value == 9911);
  assert(consume(method, 1, &value) == SIGSEGV);
  assert(method == 2 || value == 9000);
  finish(method);
}

static void fresh_native_after_dequeue(int method) {
  begin(method);
  send_packet(SIGTRAP, 9001, 0, 1);
  send_native(SIGTRAP, 9002, 0);
  hook = 4;
  int value;
  assert(consume(method, 1, &value) == SIGTRAP);
  assert(method == 2 || value == 9001);
  assert(consume(method, 1, &value) == SIGTRAP);
  assert(method == 2 || value == 9999);
  finish(method);
}

static void overlapping_iovec(void) {
  begin(4);

  union {
    struct iovec vectors[16];
    unsigned char bytes[256];
  } metadata = {0};
  struct signalfd_siginfo second;
  metadata.vectors[0] = (struct iovec){metadata.bytes, 128};
  metadata.vectors[1] = (struct iovec){&second, 128};
  send_packet(SIGTRAP, 9800, 0, 1);
  send_native(SIGUSR1, 9900, 0);
  assert(readv(descriptor, metadata.vectors, 16) == 256);
  struct signalfd_siginfo first;
  memcpy(&first, metadata.bytes, sizeof first);
  assert(first.ssi_signo == SIGTRAP && first.ssi_int == 9800);
  assert(second.ssi_signo == SIGUSR1 && second.ssi_int == 9900);
  finish(4);
}

static void invalid_copy(void) {
  begin(0);
  void *bad = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(bad != MAP_FAILED);
  assert(sigpending(bad) == -1 && errno == EFAULT);
  send_packet(SIGTRAP, 9100, 0, 1);
  struct timespec zero = {0};
  assert(sigtimedwait(&wanted, bad, &zero) == -1 && errno == EFAULT);
  assert(!signal_host_retirement_busy());

  send_packet(SIGTRAP, 9100, 0, 1);
  send_native(SIGTRAP, 9200, 0);
  assert(read(descriptor, bad, 128) == -1 && errno == EFAULT);
  assert(!signal_host_retirement_busy());
  uint64_t pending;
  assert(!syscall(SYS_rt_sigpending, &pending, 8) && !pending);
  assert(!munmap(bad, 4096));
  finish(0);
}

#ifndef __ANDROID__
static void suspended_read(void) {
  int value;
  hook = 3;
  assert(consume(3, 1, &value) == SIGTRAP && value == 9300);
  returned = 1;
}
#endif

static void native_close_context(void) {
  int pipes[2];
  assert(!pipe2(pipes, O_CLOEXEC));
  if (!sigsetjmp(escape, 1)) {
    close_hook = 1;
    close(pipes[0]);
    assert(!"close survived native jump");
  }
  assert(fcntl(pipes[0], F_GETFD) == -1 && errno == EBADF);
  assert(dup2(pipes[1], pipes[0]) == pipes[0]);

#ifndef __ANDROID__
  volatile int resumed = 0;
  assert(!getcontext(&saved));
  if (!resumed) {
    resumed = 1;
    close_hook = 2;
    close(pipes[0]);
    assert(!"close survived native context transfer");
  }
  assert(fcntl(pipes[0], F_GETFD) == -1 && errno == EBADF);
#else
  assert(!close(pipes[0]));
#endif
  assert(!close(pipes[1]));
}

static void native_continuations(void) {
  begin(0);
  send_packet(SIGTRAP, 9100, 0, 1);
  if (!sigsetjmp(escape, 1)) {
    hook = 1;
    sigwaitinfo(&wanted, NULL);
    assert(!"wait survived nonlocal transfer");
  }
  assert(!signal_host_retirement_busy());

#ifndef __ANDROID__
  volatile int resumed = 0;
  assert(!getcontext(&saved));
  if (!resumed) {
    resumed = 1;
    send_packet(SIGTRAP, 9200, 0, 1);
    hook = 2;
    struct signalfd_siginfo record;
    read(descriptor, &record, sizeof record);
    assert(!"read survived context transfer");
  }
  assert(!signal_host_retirement_busy());

  assert(!getcontext(&suspended));
  suspended.uc_stack = (stack_t){.ss_sp = continuation_stack,
                                 .ss_size = sizeof continuation_stack};
  suspended.uc_link = &caller;
  makecontext(&suspended, suspended_read, 0);
  send_packet(SIGTRAP, 9300, 0, 1);
  assert(!swapcontext(&caller, &suspended) && !returned);
  send_packet(SIGTRAP, 9400, 0, 1);
  int value;
  assert(consume(3, 1, &value) == SIGTRAP && value == 9400);
  assert(!swapcontext(&caller, &suspended) && returned);
#endif

  native_close_context();
  send_packet(SIGTRAP, 9500, 0, 1);
  int fresh;
  assert(consume(3, 1, &fresh) == SIGTRAP && fresh == 9500);
  finish(0);
}

static _Atomic long waiting_tid;
static int blocking_mode, blocking_method;
static volatile sig_atomic_t handled;

static void blocked_handler(int number) {
  assert(number == SIGUSR1);
  ++handled;
  send_packet(SIGTRAP, 9600, 0, 1);
  if (blocking_mode == 4) {
    int value;
    assert(consume(3, 1, &value) == SIGTRAP && value == 9600);
    send_packet(SIGTRAP, 9700, 0, 1);
  }
  if (blocking_mode == 2) {
    siglongjmp(escape, 1);
  }
#ifndef __ANDROID__
  if (blocking_mode == 3) {
    assert(!setcontext(&saved));
  }
#endif
}

static void *blocked_reader(void *unused) {
  (void)unused;
  begin(3);
  sigemptyset(&wanted);
  sigaddset(&wanted, SIGTRAP);
  assert(signalfd(descriptor, &wanted, 0) == descriptor);
  assert(!fcntl(descriptor, F_SETFL, 0));
  struct sigaction action = {
      .sa_handler = blocked_handler,
      .sa_flags = blocking_mode == 1 || blocking_mode == 4 ? SA_RESTART : 0};
  sigemptyset(&action.sa_mask);
  assert(!sigaction(SIGUSR1, &action, NULL));
  sigset_t interrupt;
  sigemptyset(&interrupt);
  sigaddset(&interrupt, SIGUSR1);
  assert(!pthread_sigmask(SIG_UNBLOCK, &interrupt, NULL));

  volatile int resumed = 0;
#ifndef __ANDROID__
  assert(!getcontext(&saved));
#endif
  int jumped = sigsetjmp(escape, 1);
  if (!jumped && !resumed) {
    resumed = 1;
    atomic_store_explicit(&waiting_tid, syscall(SYS_gettid),
                          memory_order_release);
    struct signalfd_siginfo record;
    struct iovec vector = {&record, sizeof record};
    errno = EDOM;
    ssize_t result = blocking_method ? readv(descriptor, &vector, 1)
                                     : read(descriptor, &record, sizeof record);
    if (blocking_mode == 0) {
      assert(result == -1 && errno == EINTR);
    } else {
      assert(blocking_mode == 1 || blocking_mode == 4);
      assert(result == sizeof record && errno == EDOM);
      assert(record.ssi_signo == SIGTRAP &&
             (int)record.ssi_int == (blocking_mode == 4 ? 9700 : 9600));
    }
  }

  assert(handled == 1);
  if (blocking_mode == 0 || blocking_mode == 2 || blocking_mode == 3) {
    int value;
    assert(consume(3, 1, &value) == SIGTRAP && value == 9600);
  }
  finish(3);
  return NULL;
}

static void blocking_case(int index) {
  blocking_mode = index / 2;
  blocking_method = index % 2;
#ifdef __ANDROID__
  if (blocking_mode == 3) {
    return; /* Bionic has no public getcontext/setcontext API. */
  }
#endif
  sigset_t all;
  sigfillset(&all);
  assert(!pthread_sigmask(SIG_BLOCK, &all, NULL));
  pthread_t thread;
  assert(!pthread_create(&thread, NULL, blocked_reader, NULL));

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
        long call;
        if (sscanf(text, "%ld", &call) == 1 && call == SYS_ppoll) {
          break;
        }
      }
    }
    assert(!clock_gettime(CLOCK_MONOTONIC, &now));
    assert(now.tv_sec - start.tv_sec < 5);
    sched_yield();
  }
  assert(!pthread_kill(thread, SIGUSR1));
  assert(!pthread_join(thread, NULL));
}

static void run(int index) {
  if (index < 24) {
    order_case(index / 4, index % 4);
  } else if (index < 30) {
    concurrent_arrival(index - 24);
  } else if (index == 30) {
    invalid_copy();
  } else if (index == 31) {
    native_continuations();
  } else if (index < 42) {
    blocking_case(index - 32);
  } else if (index < 48) {
    fresh_native_after_dequeue(index - 42);
  } else {
    overlapping_iovec();
  }
}

int main(void) {
  struct rlimit limit = {0};
  assert(!setrlimit(RLIMIT_CORE, &limit));
  for (int i = 0; i < 49; ++i) {
#ifdef __ANDROID__
    if (i == 38 || i == 39) {
      continue; /* Bionic has no public getcontext/setcontext API. */
    }
#endif
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
      run(i);
      _exit(0);
    }
    int status;
    assert(waitpid(child, &status, 0) == child);
    if (!WIFEXITED(status) || WEXITSTATUS(status)) {
      fprintf(stderr, "ordering case %d failed: status=%d\n", i, status);
      return 1;
    }
  }
  puts(
      "{\"result\":\"PASS\",\"linux_priority\":true,"
      "\"thread_before_shared\":true,\"standard_coalescing\":true,"
      "\"shared_duplicate\":true,\"rt_fifo\":true,"
      "\"higher_native_arrival\":true,\"efault_commit\":true,"
      "\"without_jump_interposer\":true,"
      "\"blocking_restart\":true,\"blocking_eintr\":true,"
      "\"blocking_native_jump\":true,\"nested_blocking_reader\":true,"
      "\"fresh_native_after_dequeue\":true,\"overlapping_iovec\":true,"
#ifdef __ANDROID__
      "\"ucontext_api_unavailable\":true}"
#else
      "\"native_context_suspend_resume\":true,\"blocking_setcontext\":true}"
#endif
  );
}
