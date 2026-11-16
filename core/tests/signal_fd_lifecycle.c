#define _GNU_SOURCE
#include "signals.h"
#include "signal-router.h"
#include "nonlocal-mask.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/wait.h>

#ifndef CLOSE_RANGE_UNSHARE
#define CLOSE_RANGE_UNSHARE (1U << 1)
#define CLOSE_RANGE_CLOEXEC (1U << 2)
#endif

extern int signal_fd_channel_busy(void);
static sigset_t wanted;
static pthread_barrier_t barrier;
static int private_error;

static void synchronize(void) {
  int result = pthread_barrier_wait(&barrier);
  assert(!result || result == PTHREAD_BARRIER_SERIAL_THREAD);
}

static int descriptor(void) {
  int fd = signalfd(-1, &wanted, SFD_NONBLOCK | SFD_CLOEXEC);
  assert(fd >= 0);
  return fd;
}

static void consume(int fd) {
  struct exchange exchange = {.host_tid = syscall(SYS_gettid)};
  struct signal_slot *slot = signal_acquire(&exchange);
  assert(slot);
  siginfo_t source = {.si_signo = SIGTRAP, .si_code = SI_QUEUE};
  source.si_pid = getpid();
  source.si_uid = getuid();
  source.si_value.sival_int = 9813;
  uint64_t previous = gw_nonlocal_mask_block();
  assert(!signal_host_defer(slot, &source));
  gw_nonlocal_mask_restore(previous);
  struct signalfd_siginfo information;
  assert(read(fd, &information, sizeof information) == sizeof information);
  assert(information.ssi_signo == SIGTRAP && information.ssi_code == SI_QUEUE &&
         information.ssi_pid == (unsigned)getpid() &&
         information.ssi_uid == getuid() && information.ssi_int == 9813);
  assert(!signal_prepare_release(slot, 0));
  signal_release(slot);
}

static void imported(int rights) {
  int source = descriptor(), alias;
  if (rights) {
    int sockets[2];
    assert(!socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0, sockets));

    union {
      struct cmsghdr align;
      unsigned char bytes[CMSG_SPACE(sizeof(int))];
    } control = {0};

    char data = 1;
    struct iovec vector = {&data, 1};
    struct msghdr message = {.msg_iov = &vector,
                             .msg_iovlen = 1,
                             .msg_control = control.bytes,
                             .msg_controllen = sizeof control.bytes};
    struct cmsghdr *header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(header), &source, sizeof source);
    assert(sendmsg(sockets[0], &message, 0) == 1);
    memset(control.bytes, 0, sizeof control.bytes);
    assert(recvmsg(sockets[1], &message, MSG_CMSG_CLOEXEC) == 1);
    header = CMSG_FIRSTHDR(&message);
    assert(header && header->cmsg_level == SOL_SOCKET &&
           header->cmsg_type == SCM_RIGHTS);
    memcpy(&alias, CMSG_DATA(header), sizeof alias);
    assert(!close(sockets[0]) && !close(sockets[1]));
  } else {
    alias = syscall(SYS_dup, source);
    assert(alias >= 0);
  }
  assert(!close(source));
  consume(alias);
  assert(signal_router_restore() == EBUSY);
  assert(!close(alias));
  assert(!signal_fd_channel_busy());
}

static int range_close(void) {
#ifdef SYS_close_range
  int fd = descriptor();
  if (syscall(SYS_close_range, fd, fd, CLOSE_RANGE_CLOEXEC)) {
    int error = errno;
    assert(!close(fd));
    return error;
  }
  assert(fcntl(fd, F_GETFD) & FD_CLOEXEC);
  assert(signal_fd_channel_busy() > 0);
  consume(fd);
  assert(!syscall(SYS_close_range, fd, fd, 0));
  assert(!signal_fd_channel_busy());
  return 0;
#else
  return ENOSYS;
#endif
}

static void *private_table(void *argument) {
  int fd = *(int *)argument;
#ifdef SYS_close_range
  /* An empty high range still creates a private files_struct, without closing
   * unrelated descriptors owned by the test runner.
   * https://github.com/torvalds/linux/blob/v6.12/fs/file.c
   */
  if (syscall(SYS_close_range, UINT_MAX, UINT_MAX, CLOSE_RANGE_UNSHARE)) {
    int error = errno;
    if (error != ENOSYS || syscall(SYS_unshare, CLONE_FILES)) {
      private_error = error == ENOSYS ? errno : error;
    }
  }
#else
  private_error = ENOSYS;
#endif
  synchronize();
  synchronize();
  if (!private_error) {
    consume(fd);
    assert(!syscall(SYS_close, fd));
  }
  return NULL;
}

static int private_lifetime(void) {
  int fd = descriptor();
  assert(!pthread_barrier_init(&barrier, NULL, 2));
  pthread_t thread;
  assert(!pthread_create(&thread, NULL, private_table, &fd));
  synchronize();
  assert(!close(fd));
  if (!private_error) {
    /* The calling thread no longer has this descriptor. A different thread's
     * private table still retains the OFD and its private notification mask. */
    assert(signal_router_restore() == EBUSY);
  }
  synchronize();
  assert(!pthread_join(thread, NULL));
  assert(!pthread_barrier_destroy(&barrier));
  assert(!signal_fd_channel_busy());
  return private_error;
}

static int run_case(const char *name) {
  sigemptyset(&wanted);
  sigaddset(&wanted, SIGTRAP);
  assert(!pthread_sigmask(SIG_BLOCK, &wanted, NULL));
  assert(!signal_router_init());
  const int notifications[] = {SIGRTMIN,     SIGRTMIN + 1, SIGRTMIN + 2,
                               SIGRTMIN + 3, SIGRTMIN + 4, SIGRTMIN + 5};
  if (!strcmp(name, "untracked-before-enable")) {
    uint64_t mask = UINT64_C(1) << (SIGTRAP - 1);
    int fd = syscall(SYS_signalfd4, -1, &mask, 8, SFD_CLOEXEC);
    assert(fd >= 0);
    assert(signal_router_defer_faults(notifications) == EBUSY);
    assert(!syscall(SYS_close, fd));
  }
  assert(!signal_router_defer_faults(notifications));
  int error = 0;
  if (!strcmp(name, "raw-duplicate")) {
    imported(0);
  } else if (!strcmp(name, "scm-rights")) {
    imported(1);
  } else if (!strcmp(name, "close-range")) {
    error = range_close();
  } else if (!strcmp(name, "private-table")) {
    error = private_lifetime();
  }
  assert(!signal_router_restore());
  assert(!error || error == ENOSYS || error == EPERM || error == EINVAL);
  return error;
}

int main(void) {
  struct rlimit limit = {0};
  assert(!setrlimit(RLIMIT_CORE, &limit));
  const char *names[] = {"raw-duplicate", "scm-rights", "close-range",
                         "private-table", "untracked-before-enable"};
  unsigned skipped = 0;
  for (unsigned i = 0; i < sizeof names / sizeof names[0]; ++i) {
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
      int error = run_case(names[i]);
      printf(
          "{\"stage\":\"fd-lifecycle\",\"case\":\"%s\","
          "\"result\":\"%s\",\"native_error\":%d}\n",
          names[i], error ? "UNAVAILABLE" : "PASS", error);
      fflush(stdout);
      _exit(error ? 77 : 0);
    }
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status));
    int code = WEXITSTATUS(status);
    assert(code == 0 || code == 77);
    skipped += code == 77;
  }
  printf(
      "{\"result\":\"PASS\",\"fd_lifecycle_cases\":5,"
      "\"native_unavailable\":%u}\n",
      skipped);
}
