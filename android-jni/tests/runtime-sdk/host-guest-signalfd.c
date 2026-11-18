#define _GNU_SOURCE
#include "guest-fd-probe.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/select.h>
#include <sys/signalfd.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

static _Thread_local volatile sig_atomic_t job_handlers;
static _Atomic int job_total;
static int job_gate;

static void job_handler(int number) {
  assert(number == SIGTSTP);
  ++job_handlers;
  atomic_fetch_add_explicit(&job_total, 1, memory_order_relaxed);
}

static _Noreturn void supervise(pid_t child, int gate) {
  const int expected[] = {SIGTSTP, SIGTTIN, SIGTTOU};
  unsigned stops = 0;
  struct timespec start, now;
  assert(!clock_gettime(CLOCK_MONOTONIC, &start));
  for (;;) {
    int status;
    pid_t observed = waitpid(child, &status, WNOHANG | WUNTRACED);
    if (observed == child && WIFSTOPPED(status)) {
      if (stops >= sizeof expected / sizeof *expected ||
          WSTOPSIG(status) != expected[stops] || kill(child, SIGCONT) ||
          write(gate, "c", 1) != 1) {
        fprintf(stderr, "Unexpected Guest stop signal %d at index %u\n",
                WSTOPSIG(status), stops);
        break;
      }
      ++stops;
    } else if (observed == child) {
      if (WIFEXITED(status) && !WEXITSTATUS(status) &&
          stops == sizeof expected / sizeof *expected) {
        puts(
            "{\"stage\":\"guest-job-control\",\"native_stops\":3,"
            "\"resumed\":true,\"host_disposition\":true}");
        fflush(stdout);
        _exit(0);
      }
      _exit(1);
    } else if (observed < 0 && errno != EINTR) {
      break;
    }

    assert(!clock_gettime(CLOCK_MONOTONIC, &now));
    if (now.tv_sec - start.tv_sec >= 30) {
      break;
    }
    struct timespec pause = {0, 1000000};
    nanosleep(&pause, NULL);
  }

  kill(child, SIGKILL);
  while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {
  }
  fprintf(stderr, "Guest job-control child failed after %u kernel stops\n",
          stops);
  _exit(1);
}

void sdk_job_control_child(void) {
  /* Fork while single-threaded, before either VM or Guest runtime starts.
   * Separate process groups in the same session keep the child's group
   * non-orphaned, so Linux applies the normal job-control stop actions. */
  int pipe[2];
  assert(!pipe2(pipe, O_CLOEXEC));
  pid_t child = fork();
  assert(child >= 0);
  if (child) {
    assert(!close(pipe[0]));
    supervise(child, pipe[1]);
  }
  assert(!close(pipe[1]));
  job_gate = pipe[0];
  assert(!setpgid(0, 0));
}

static jint invoke(JNIEnv *env, jclass cls, jmethodID method, int op, int arg) {
  jint result = (*env)->CallStaticIntMethod(env, cls, method, op, arg);
  assert(!(*env)->ExceptionCheck(env));
  return result;
}

static void check_packet(const struct signalfd_siginfo *packet, int signo,
                         int value) {
  assert(packet->ssi_signo == (uint32_t)signo && packet->ssi_code == SI_QUEUE);
  assert(packet->ssi_pid == (uint32_t)getpid() && packet->ssi_uid == getuid());
  assert(packet->ssi_int == value);
}

static void check_readiness(int descriptor) {
  struct pollfd watch = {.fd = descriptor, .events = POLLIN};
  assert(poll(&watch, 1, 0) == 1 && watch.revents == POLLIN);
  assert(descriptor < FD_SETSIZE);
  fd_set readable;
  FD_ZERO(&readable);
  FD_SET(descriptor, &readable);
  struct timespec zero = {0};
  assert(pselect(descriptor + 1, &readable, NULL, NULL, &zero, NULL) == 1);
  assert(FD_ISSET(descriptor, &readable));
}

struct reader {
  struct pollfd watch;
  struct signalfd_siginfo packet;
  _Atomic int tid;
};

static void *read_when_ready(void *opaque) {
  struct reader *reader = opaque;
  atomic_store_explicit(&reader->tid, syscall(SYS_gettid),
                        memory_order_release);
  assert(poll(&reader->watch, 1, 3000) == 1);
  if (reader->watch.revents == POLLIN) {
    assert(read(reader->watch.fd, &reader->packet, sizeof reader->packet) ==
           sizeof reader->packet);
  }
  return NULL;
}

static void wait_for_call(const _Atomic int *recipient, long expected) {
  struct timespec start, now;
  assert(!clock_gettime(CLOCK_MONOTONIC, &start));
  for (;;) {
    int tid = atomic_load_explicit(recipient, memory_order_acquire);
    if (tid) {
      char path[96];
      snprintf(path, sizeof path, "/proc/self/task/%d/syscall", tid);
      FILE *file = fopen(path, "r");
      assert(file);
      long number;
      int waiting = fscanf(file, "%ld", &number) == 1 && number == expected;
      assert(!fclose(file));
      if (waiting) {
        return;
      }
    }
    assert(!clock_gettime(CLOCK_MONOTONIC, &now));
    assert(now.tv_sec - start.tv_sec < 2);
    sched_yield();
  }
}

static pthread_t start_reader(int fd, struct reader *reader) {
  *reader = (struct reader){.watch = {.fd = fd, .events = POLLIN}};
  pthread_t thread;
  assert(!pthread_create(&thread, NULL, read_when_ready, reader));
  wait_for_call(&reader->tid, SYS_ppoll);
  return thread;
}

static void check_directed(JNIEnv *env, jclass cls, jmethodID method, int fd,
                           int worker) {
  assert(!invoke(env, cls, method, SDK_FD_DIRECTED, 511));
  struct signalfd_siginfo packet;
  if (worker) {
    assert(read(fd, &packet, sizeof packet) == -1 && errno == EAGAIN);
    assert(!invoke(env, cls, method, SDK_FD_DRAIN_DIRECTED, fd));
  } else {
    check_readiness(fd);
    struct iovec vector = {&packet, sizeof packet};
    assert(readv(fd, &vector, 1) == sizeof packet);
    check_packet(&packet, SDK_FD_PACKET, 511);
  }
}

static void check_shared(JNIEnv *env, jclass cls, jmethodID method, int fd,
                         int stdio) {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SDK_FD_OTHER);
  int alias = dup(fd);
  assert(alias >= 0 && signalfd(alias, &set, 0) == alias);
  int epoll = epoll_create1(EPOLL_CLOEXEC);
  struct epoll_event event = {.events = EPOLLIN, .data.u64 = 712};
  assert(epoll >= 0 && !epoll_ctl(epoll, EPOLL_CTL_ADD, fd, &event));
  assert(!invoke(env, cls, method, SDK_FD_SHARED, 521));
  check_readiness(fd);
  assert(epoll_wait(epoll, &event, 1, 0) == 1 && event.data.u64 == 712);
  struct signalfd_siginfo packet;
  assert(read(alias, &packet, sizeof packet) == sizeof packet);
  check_packet(&packet, SDK_FD_OTHER, 521);
  assert(!close(epoll));

  struct reader reader;
  pthread_t thread = start_reader(alias, &reader);
  assert(!invoke(env, cls, method, SDK_FD_SHARED, 522));
  assert(!pthread_join(thread, NULL) && reader.watch.revents == POLLIN);
  check_packet(&reader.packet, SDK_FD_OTHER, 522);
  assert(!close(alias));

  if (stdio) {
    alias = dup(fd);
    assert(alias >= 0);
    FILE *file = fdopen(alias, "r");
    assert(file && fileno(file) == alias);
    assert(!invoke(env, cls, method, SDK_FD_SHARED, 523));
    assert(fread(&packet, sizeof packet, 1, file) == 1);
    check_packet(&packet, SDK_FD_OTHER, 523);
    assert(!fclose(file));
  }
}

static void check_close(int fd) {
  struct reader reader;
  pthread_t thread = start_reader(fd, &reader);
  assert(!close(fd));
  assert(!pthread_join(thread, NULL) && reader.watch.revents == POLLNVAL);
}

static _Thread_local sigjmp_buf read_return;

static void abandon_read(int number) {
  assert(number == SIGURG);
  siglongjmp(read_return, 1);
}

static void *read_directly(void *opaque) {
  struct reader *reader = opaque;
  if (sigsetjmp(read_return, 1)) {
    return reader;
  }
  atomic_store_explicit(&reader->tid, syscall(SYS_gettid),
                        memory_order_release);
  assert(read(reader->watch.fd, &reader->packet, sizeof reader->packet) ==
         sizeof reader->packet);
  return NULL;
}

static pthread_t start_direct_read(int fd, struct reader *reader) {
  *reader = (struct reader){.watch = {.fd = fd}};
  pthread_t thread;
  assert(!pthread_create(&thread, NULL, read_directly, reader));
  wait_for_call(&reader->tid, SYS_futex);
  return thread;
}

static void check_blocking_read(JNIEnv *env, jclass cls, jmethodID method) {
  int fd = invoke(env, cls, method, SDK_FD_OPEN, 1);
  assert(fd >= 0);
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SDK_FD_OTHER);
  assert(signalfd(fd, &mask, 0) == fd);

  struct reader reader;
  pthread_t thread = start_direct_read(fd, &reader);
  assert(!syscall(SYS_tgkill, getpid(), reader.tid, 54));
  assert(!invoke(env, cls, method, SDK_FD_SHARED, 531));
  void *result;
  assert(!pthread_join(thread, &result) && !result);
  check_packet(&reader.packet, SDK_FD_OTHER, 531);

  struct sigaction action = {.sa_handler = abandon_read}, previous;
  sigemptyset(&action.sa_mask);
  assert(!sigaction(SIGURG, &action, &previous));
  thread = start_direct_read(fd, &reader);
  assert(!pthread_kill(thread, SIGURG));
  assert(!pthread_join(thread, &result) && result == &reader);
  assert(!sigaction(SIGURG, &previous, NULL));

#ifndef __ANDROID__
  thread = start_direct_read(fd, &reader);
  assert(!pthread_cancel(thread));
  assert(!pthread_join(thread, &result) && result == PTHREAD_CANCELED);
#endif

  assert(!close(fd));
  assert(!invoke(env, cls, method, SDK_FD_FINISH, 0));
}

struct interruption {
  _Atomic int tid;
  int restart, completed;
};

static void *interrupt_guest_read(void *opaque) {
  struct interruption *test = opaque;
  wait_for_call(&test->tid, SYS_ppoll);
  assert(!syscall(SYS_tgkill, getpid(), test->tid, SIGTSTP));
  if (test->restart) {
    struct timespec start, now;
    assert(!clock_gettime(CLOCK_MONOTONIC, &start));
    while (atomic_load_explicit(&job_total, memory_order_relaxed) ==
           test->completed) {
      assert(!clock_gettime(CLOCK_MONOTONIC, &now));
      assert(now.tv_sec - start.tv_sec < 2);
      sched_yield();
    }
    wait_for_call(&test->tid, SYS_ppoll);
    assert(!syscall(SYS_tgkill, getpid(), test->tid, SIGTTIN));
  }
  return NULL;
}

static void check_job_read_restart(JNIEnv *env, jclass cls, jmethodID method,
                                   struct sigaction action) {
  jmethodID tid = (*env)->GetStaticMethodID(env, cls, "tid", "()J");
  assert(tid);
  jlong recipient = (*env)->CallStaticLongMethod(env, cls, tid);
  assert(recipient > 0 && recipient <= INT32_MAX &&
         !(*env)->ExceptionCheck(env));
  for (int restart = 0; restart <= 1; ++restart) {
    action.sa_flags = restart ? SA_RESTART : 0;
    assert(!sigaction(SIGTSTP, &action, NULL));
    struct interruption test = {.tid = (int)recipient,
                                .restart = restart,
                                .completed = atomic_load(&job_total)};
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, interrupt_guest_read, &test));
    assert(!invoke(env, cls, method, SDK_FD_JOB_READ, restart));
    assert(!pthread_join(thread, NULL));
    assert(atomic_load(&job_total) == test.completed + 1);
  }
}

void sdk_host_guest_fds(JNIEnv *env, jclass cls, int worker, int stdio) {
  jmethodID method =
      (*env)->GetStaticMethodID(env, cls, "guestDescriptor", "(II)I");
  assert(method);
  int fd = invoke(env, cls, method, SDK_FD_OPEN, 0);
  assert(fd >= 0);
  check_directed(env, cls, method, fd, worker);
  check_shared(env, cls, method, fd, stdio);
  check_close(fd);
  assert(!invoke(env, cls, method, SDK_FD_FINISH, 0));
  check_blocking_read(env, cls, method);
  printf(
      "{\"stage\":\"host-guest-signalfd\",\"directed_owner\":true,"
      "\"shared_wait\":true,\"close_wake\":true,\"blocked_cleanup\":true,"
      "\"stdio\":%s}\n",
      stdio ? "true" : "false");
  fflush(stdout);

  struct sigaction action = {.sa_handler = job_handler}, previous;
  assert(!sigemptyset(&action.sa_mask));
  assert(!sigaction(SIGTSTP, &action, &previous));
  sigset_t set, inherited;
  sigemptyset(&set);
  sigaddset(&set, SIGTSTP);
  assert(!pthread_sigmask(SIG_UNBLOCK, &set, &inherited));
  assert(!invoke(env, cls, method, SDK_FD_JOB_HANDLER, 0));
  assert(atomic_load_explicit(&job_total, memory_order_relaxed) == 1);
  if (!worker) {
    assert(job_handlers == 1);
  }
  struct sigaction observed;
  assert(!sigaction(SIGTSTP, NULL, &observed));
  assert(observed.sa_handler == job_handler);
  check_job_read_restart(env, cls, method, action);
  assert(!sigaction(SIGTSTP, &previous, NULL));
  assert(!invoke(env, cls, method, SDK_FD_JOB_STOP, job_gate));
  assert(!close(job_gate));
  assert(!pthread_sigmask(SIG_SETMASK, &inherited, NULL));
}
