#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/select.h>
#include <sys/signalfd.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

enum { PACKET = 56, OTHER = 57, NATIVE_EVENT = 13, SIGNAL_EVENT = 29 };

static sigset_t signal_set(int number) {
  sigset_t set;
  assert(!sigemptyset(&set) && !sigaddset(&set, number));
  return set;
}

static int signal_fd(void) {
  sigset_t set = signal_set(PACKET);
  int fd = signalfd(-1, &set, SFD_CLOEXEC | SFD_NONBLOCK);
  assert(fd >= 0);
  return fd;
}

static void enqueue(pthread_t thread, int number, int payload) {
  assert(
      !pthread_sigqueue(thread, number, (union sigval){.sival_int = payload}));
}

static void take(int fd, int number, int payload) {
  struct signalfd_siginfo info;
  assert(read(fd, &info, sizeof info) == sizeof info);
  assert(info.ssi_signo == (uint32_t)number && info.ssi_int == payload);
}

static int event_fd(void) {
  int fd = epoll_create1(EPOLL_CLOEXEC);
  assert(fd >= 0);
  return fd;
}

static void control(int epfd, int op, int fd, uint32_t flags, uint64_t data) {
  struct epoll_event event = {.events = flags, .data.u64 = data};
  assert(!epoll_ctl(epfd, op, fd, &event));
}

static void event_is(int epfd, uint64_t data) {
  struct epoll_event event;
  assert(epoll_wait(epfd, &event, 1, 0) == 1);
  assert(event.events == EPOLLIN && event.data.u64 == data);
}

static void poll_select(void) {
  int fd = signal_fd();
  int pipes[2];
  assert(!pipe2(pipes, O_CLOEXEC));

  struct pollfd fds[32] = {{.fd = fd, .events = POLLIN | POLLOUT},
                           {.fd = fd, .events = POLLRDNORM},
                           {.fd = pipes[0], .events = POLLIN}};
  for (size_t i = 3; i < sizeof fds / sizeof *fds; ++i) {
    fds[i] = (struct pollfd){.fd = -1, .events = POLLIN, .revents = POLLNVAL};
  }
  assert(poll(fds, sizeof fds / sizeof *fds, 0) == 0 && !fds[3].revents);
  assert(write(pipes[1], "x", 1) == 1);

  enqueue(pthread_self(), PACKET, 201);
  const struct timespec zero = {0};
  assert(ppoll(fds, sizeof fds / sizeof *fds, &zero, NULL) == 2);
  assert(fds[0].revents == POLLIN && !fds[1].revents &&
         fds[2].revents == POLLIN);

  fd_set r, w, x;
  FD_ZERO(&r);
  FD_ZERO(&w);
  FD_ZERO(&x);
  FD_SET(fd, &r);
  FD_SET(fd, &w);
  FD_SET(fd, &x);
  FD_SET(pipes[0], &r);
  FD_SET(pipes[1], &w);
  int maximum = (pipes[1] > fd ? pipes[1] : fd) + 1;
  assert(pselect(maximum, &r, &w, &x, &zero, NULL) == 3);
  assert(FD_ISSET(fd, &r) && !FD_ISSET(fd, &w) && !FD_ISSET(fd, &x));
  assert(FD_ISSET(pipes[0], &r) && FD_ISSET(pipes[1], &w));

  int high = fcntl(fd, F_DUPFD_CLOEXEC, FD_SETSIZE + 16);
  assert(high >= FD_SETSIZE + 16);
  unsigned long bits[high / (8 * sizeof(unsigned long)) + 1];
  memset(bits, 0, sizeof bits);
  bits[high / (8 * sizeof(unsigned long))] =
      1UL << (high % (8 * sizeof(unsigned long)));
  assert(pselect(high + 1, (fd_set *)bits, NULL, NULL, &zero, NULL) == 1);
  assert(bits[high / (8 * sizeof(unsigned long))] ==
         1UL << (high % (8 * sizeof(unsigned long))));
  take(fd, PACKET, 201);
  assert(!close(high));

  FD_ZERO(&r);
  FD_SET(fd, &r);
  struct timeval delay = {.tv_usec = 2000};
  assert(select(fd + 1, &r, NULL, NULL, &delay) == 0);
  assert(!delay.tv_sec && !delay.tv_usec && !FD_ISSET(fd, &r));
  FD_SET(fd, &r);
  assert(!close(fd) && !close(pipes[1]) && !close(pipes[0]));
}

static void modes_and_aliases(void) {
  int fd = signal_fd(), ep = event_fd();
  int alias = dup(fd), ep_alias = dup(ep);
  assert(alias >= 0 && ep_alias >= 0);

  control(ep, EPOLL_CTL_ADD, fd, EPOLLIN | EPOLLOUT | EPOLLET, UINT64_MAX);
  assert(epoll_ctl(ep, EPOLL_CTL_ADD, fd,
                   &(struct epoll_event){.events = EPOLLIN}) == -1 &&
         errno == EEXIST);
  control(ep_alias, EPOLL_CTL_ADD, alias, EPOLLIN | EPOLLONESHOT, SIGNAL_EVENT);
  struct epoll_event events[4];
  assert(epoll_wait(ep_alias, events, 4, 0) == 0);

  enqueue(pthread_self(), PACKET, 211);
  assert(epoll_wait(ep_alias, events, 4, 0) == 2);
  assert(events[0].events == EPOLLIN && events[1].events == EPOLLIN);
  assert(
      (events[0].data.u64 == SIGNAL_EVENT &&
       events[1].data.u64 == UINT64_MAX) ||
      (events[1].data.u64 == SIGNAL_EVENT && events[0].data.u64 == UINT64_MAX));
  assert(epoll_wait(ep, events, 4, 0) == 0);

  /* The native signalfd waitqueue fans out even on an unrelated enqueue. */
  enqueue(pthread_self(), OTHER, 215);
  event_is(ep, UINT64_MAX);
  sigset_t other = signal_set(OTHER);
  assert(sigtimedwait(&other, NULL, &(struct timespec){0}) == OTHER);
  sigset_t original = signal_set(PACKET);
  assert(signalfd(alias, &original, 0) == alias);
  event_is(ep, UINT64_MAX);

  enqueue(pthread_self(), PACKET, 212);
  event_is(ep, UINT64_MAX);
  assert(epoll_wait(ep, events, 4, 0) == 0);
  control(ep, EPOLL_CTL_MOD, alias, EPOLLIN | EPOLLONESHOT, SIGNAL_EVENT);
  event_is(ep_alias, SIGNAL_EVENT);
  take(alias, PACKET, 211);
  take(fd, PACKET, 212);

  assert(!close(ep));
  int fresh = event_fd();
  assert(fresh >= 0 && epoll_wait(fresh, events, 4, 0) == 0);
  int pipes[2];
  assert(!pipe2(pipes, O_CLOEXEC));
  assert(dup2(pipes[0], fd) == fd);
  control(ep_alias, EPOLL_CTL_ADD, fd, EPOLLIN, NATIVE_EVENT);
  assert(write(pipes[1], "a", 1) == 1);
  enqueue(pthread_self(), PACKET, 214);
  assert(epoll_wait(ep_alias, events, 4, 0) == 2);
  assert(events[0].data.u64 != events[1].data.u64);
  assert(
      (events[0].data.u64 == NATIVE_EVENT ||
       events[1].data.u64 == NATIVE_EVENT) &&
      (events[0].data.u64 == UINT64_MAX || events[1].data.u64 == UINT64_MAX));

  /* fd still holds the replacement while the final original alias closes. */
  assert(!close(alias));
  event_is(ep_alias, NATIVE_EVENT);
  assert(epoll_wait(fresh, events, 4, 0) == 0);
  sigset_t set = signal_set(PACKET);
  siginfo_t info;
  assert(sigtimedwait(&set, &info, &(struct timespec){0}) == PACKET);
  assert(info.si_value.sival_int == 214);
  assert(!close(fresh) && !close(ep_alias));
  assert(!close(fd) && !close(pipes[0]) && !close(pipes[1]));
}

static void nested_watches(void) {
  int root = event_fd(), child = event_fd(), leaf = event_fd();
  int fd = signal_fd(), pipes[2];
  assert(!pipe2(pipes, O_CLOEXEC));
  control(root, EPOLL_CTL_ADD, child, EPOLLIN | EPOLLONESHOT, 1);
  control(child, EPOLL_CTL_ADD, leaf, EPOLLIN, 2);
  control(leaf, EPOLL_CTL_ADD, fd, EPOLLIN, SIGNAL_EVENT);
  control(leaf, EPOLL_CTL_ADD, pipes[0], EPOLLIN, NATIVE_EVENT);
  assert(epoll_ctl(leaf, EPOLL_CTL_ADD, root,
                   &(struct epoll_event){.events = EPOLLIN}) == -1 &&
         errno == ELOOP);

  enqueue(pthread_self(), PACKET, 221);
  struct pollfd pollfd = {.fd = root, .events = POLLIN};
  assert(poll(&pollfd, 1, 0) == 1);
  assert(write(pipes[1], "b", 1) == 1);
  event_is(root, 1);
  struct epoll_event events[2];
  assert(epoll_wait(root, events, 2, 0) == 0);
  control(root, EPOLL_CTL_MOD, child, EPOLLIN | EPOLLONESHOT, 3);
  event_is(root, 3);
  event_is(child, 2);
  assert(epoll_wait(leaf, events, 2, 0) == 2);
  assert(events[0].data.u64 != events[1].data.u64);
  take(fd, PACKET, 221);
  char byte;
  assert(read(pipes[0], &byte, 1) == 1 && byte == 'b');
  assert(epoll_wait(child, events, 2, 0) == 0);

  control(root, EPOLL_CTL_MOD, child, EPOLLIN | EPOLLET, 4);
  enqueue(pthread_self(), PACKET, 222);
  event_is(root, 4);
  assert(epoll_wait(root, events, 2, 0) == 0);
  control(leaf, EPOLL_CTL_MOD, fd, EPOLLIN, SIGNAL_EVENT);
  event_is(root, 4);
  assert(epoll_wait(root, events, 2, 0) == 0);
  take(fd, PACKET, 222);

  assert(!close(fd) && !close(leaf) && !close(child) && !close(root));
  assert(!close(pipes[0]) && !close(pipes[1]));
}

enum wait_api { POLL, SELECT, EPOLL, EPOLL_NANOS };

struct sleeping_wait {
  int fd, signal, result, error, temporary_mask;
  enum wait_api api;
  _Atomic int tid, complete;
  struct epoll_event event;
};

static void *wait_once(void *argument) {
  struct sleeping_wait *state = argument;
  atomic_store_explicit(&state->tid, (int)syscall(SYS_gettid),
                        memory_order_release);

  sigset_t blocked, temporary;
  const sigset_t *change = NULL;
  if (state->temporary_mask) {
    assert(!pthread_sigmask(SIG_SETMASK, NULL, &blocked));
    assert(sigismember(&blocked, SIGURG) == 1);
    temporary = blocked;
    assert(!sigdelset(&temporary, SIGURG));
    change = &temporary;
  }
  struct timespec time = {.tv_sec = 4};
  if (state->api == POLL) {
    struct pollfd fd = {.fd = state->fd, .events = POLLIN};
    state->result = ppoll(&fd, 1, &time, change);
    state->event.events = fd.revents;
  } else if (state->api == SELECT) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(state->fd, &set);
    state->result = pselect(state->fd + 1, &set, NULL, NULL, &time, change);
  } else if (state->api == EPOLL) {
    state->result = change
                        ? epoll_pwait(state->fd, &state->event, 1, 4000, change)
                        : epoll_wait(state->fd, &state->event, 1, 4000);
  } else {
    state->result = epoll_pwait2(state->fd, &state->event, 1, &time, change);
  }

  state->error = errno;
  if (change) {
    assert(!pthread_sigmask(SIG_SETMASK, NULL, &blocked));
    assert(sigismember(&blocked, SIGURG) == 1);
  }
  if (state->result == 1 && state->signal) {
    take(state->signal, PACKET, state->signal);
  }
  atomic_store_explicit(&state->complete, 1, memory_order_release);
  return NULL;
}

static void wait_for_ppoll(struct sleeping_wait *state) {
  struct timespec start, now;
  assert(!clock_gettime(CLOCK_MONOTONIC, &start));
  for (;;) {
    int tid = atomic_load_explicit(&state->tid, memory_order_acquire);
    if (tid) {
      char path[80];
      assert(snprintf(path, sizeof path, "/proc/self/task/%d/syscall", tid) >
             0);
      FILE *file = fopen(path, "r");
      assert(file);
      long number;
      int asleep = fscanf(file, "%ld", &number) == 1 && number == SYS_ppoll;
      assert(!fclose(file));
      if (asleep) {
        return;
      }
    }
    assert(!atomic_load_explicit(&state->complete, memory_order_acquire));
    assert(!clock_gettime(CLOCK_MONOTONIC, &now) &&
           now.tv_sec - start.tv_sec < 4);
    sched_yield();
  }
}

static void separate_targets(enum wait_api api) {
  int fd = signal_fd(), epfd = event_fd();
  control(epfd, EPOLL_CTL_ADD, fd, EPOLLIN, SIGNAL_EVENT);
  int waited = api < EPOLL ? fd : epfd;
  struct sleeping_wait a = {.fd = waited, .signal = fd, .api = api};
  struct sleeping_wait b = a;
  pthread_t ta, tb;
  assert(!pthread_create(&ta, NULL, wait_once, &a));
  assert(!pthread_create(&tb, NULL, wait_once, &b));
  wait_for_ppoll(&a);
  wait_for_ppoll(&b);

  enqueue(ta, PACKET, fd);
  assert(!pthread_join(ta, NULL) && a.result == 1);
  assert(!atomic_load_explicit(&b.complete, memory_order_acquire));
  wait_for_ppoll(&b);
  enqueue(tb, PACKET, fd);
  assert(!pthread_join(tb, NULL) && b.result == 1);

  struct sleeping_wait cancelled = {.fd = waited, .api = api};
  pthread_t thread;
  assert(!pthread_create(&thread, NULL, wait_once, &cancelled));
  wait_for_ppoll(&cancelled);
  assert(!pthread_cancel(thread));
  void *result;
  assert(!pthread_join(thread, &result) && result == PTHREAD_CANCELED);
  assert(!close(fd) && !close(epfd));
}

static _Atomic unsigned interruptions;

static void interrupt_count(int number) {
  assert(number == SIGURG);
  atomic_fetch_add_explicit(&interruptions, 1, memory_order_relaxed);
}

static void check_cancel_mask(void *unused) {
  (void)unused;
  sigset_t mask;
  assert(!pthread_sigmask(SIG_SETMASK, NULL, &mask));
  assert(sigismember(&mask, SIGURG) == 1 && sigismember(&mask, SIGWINCH) == 0);
}

static void *cancel_mask_wait(void *argument) {
  pthread_cleanup_push(check_cancel_mask, NULL);
  wait_once(argument);
  pthread_cleanup_pop(0);
  return NULL;
}

static void temporary_masks(void) {
  sigset_t mask = signal_set(SIGURG), inherited;
  assert(!pthread_sigmask(SIG_BLOCK, &mask, &inherited));
  mask = signal_set(SIGWINCH);
  assert(!pthread_sigmask(SIG_UNBLOCK, &mask, NULL));
  struct sigaction action = {.sa_handler = interrupt_count,
                             .sa_flags = SA_RESTART};
  struct sigaction previous;
  sigemptyset(&action.sa_mask);
  assert(!sigaction(SIGURG, &action, &previous));

  int fd = signal_fd(), epfd = event_fd();
  control(epfd, EPOLL_CTL_ADD, fd, EPOLLIN, SIGNAL_EVENT);
  for (enum wait_api api = POLL; api <= EPOLL_NANOS; ++api) {
    struct sleeping_wait call = {
        .fd = api < EPOLL ? fd : epfd, .api = api, .temporary_mask = 1};
    pthread_t thread;
    interruptions = 0;
    assert(!pthread_create(&thread, NULL, wait_once, &call));
    wait_for_ppoll(&call);
    assert(!pthread_kill(thread, SIGURG));
    assert(!pthread_join(thread, NULL));
    assert(call.result == -1 && call.error == EINTR && interruptions == 1);

    /* An already-ready descriptor wins over the interruption, but the
     * pending handler must run using the temporary mask before the return. */
    interruptions = 0;
    assert(!pthread_kill(pthread_self(), SIGURG));
    enqueue(pthread_self(), PACKET, 223);
    wait_once(&call);
    assert(call.result == 1 && interruptions == 1);
    take(fd, PACKET, 223);

    atomic_store_explicit(&call.tid, 0, memory_order_relaxed);
    atomic_store_explicit(&call.complete, 0, memory_order_relaxed);
    assert(!pthread_create(&thread, NULL, cancel_mask_wait, &call));
    wait_for_ppoll(&call);
    assert(!pthread_cancel(thread));
    void *result;
    assert(!pthread_join(thread, &result) && result == PTHREAD_CANCELED);
  }
  assert(!close(fd) && !close(epfd));
  assert(!sigaction(SIGURG, &previous, NULL));
  assert(!pthread_sigmask(SIG_SETMASK, &inherited, NULL));
}

static __thread sigjmp_buf escape;
static __thread ucontext_t restore_point;
static __thread int use_context;

static void escape_wait(int number) {
  assert(number == SIGURG);
  if (use_context) {
    assert(!setcontext(&restore_point));
  }
  siglongjmp(escape, 1);
}

static void *abandon_wait(void *argument) {
  struct sleeping_wait *call = argument;
  use_context = call->api == EPOLL_NANOS;
  if (use_context) {
    assert(!getcontext(&restore_point));
  }
  if (!sigsetjmp(escape, 1) &&
      !atomic_load_explicit(&call->tid, memory_order_acquire)) {
    wait_once(call);
    assert(0 && "wait returned instead of restoring its caller");
  }

  atomic_store_explicit(&call->complete, 1, memory_order_release);
  while (atomic_load_explicit(&call->complete, memory_order_acquire) == 1) {
    sched_yield();
  }
  return NULL;
}

static void check_abandoned_waits(void) {
  sigset_t mask = signal_set(SIGURG), inherited;
  assert(!pthread_sigmask(SIG_UNBLOCK, &mask, &inherited));
  struct sigaction action = {.sa_handler = escape_wait};
  struct sigaction previous;
  sigemptyset(&action.sa_mask);
  assert(!sigaction(SIGURG, &action, &previous));

  int inspector = event_fd();
  for (enum wait_api api = EPOLL; api <= EPOLL_NANOS; ++api) {
    struct sleeping_wait call = {.fd = event_fd(), .api = api};
    struct flock lock = {
        .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 291, .l_len = 1};
    assert(!fcntl(call.fd, F_OFD_SETLK, &lock));
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, abandon_wait, &call));
    wait_for_ppoll(&call);
    assert(!close(call.fd));
    assert(!pthread_kill(thread, SIGURG));
    while (!atomic_load_explicit(&call.complete, memory_order_acquire)) {
      sched_yield();
    }

    /* Check final fput before thread teardown can release a leaked lease. */
    assert(!fcntl(inspector, F_OFD_GETLK, &lock) && lock.l_type == F_UNLCK);
    atomic_store_explicit(&call.complete, 2, memory_order_release);
    assert(!pthread_join(thread, NULL));
  }
  assert(!close(inspector));
  assert(!sigaction(SIGURG, &previous, NULL));
  assert(!pthread_sigmask(SIG_SETMASK, &inherited, NULL));
}

static void close_during_wait(void) {
  int fd = signal_fd(), epfd = event_fd();
  struct sleeping_wait call = {.fd = epfd, .signal = fd, .api = EPOLL};
  pthread_t thread;
  assert(!pthread_create(&thread, NULL, wait_once, &call));
  wait_for_ppoll(&call);

  /* An ADD must wake a wait that entered an empty epoll. Closing/replacing
   * the last user alias afterwards must not retarget the in-flight wait. */
  control(epfd, EPOLL_CTL_ADD, fd, EPOLLIN, SIGNAL_EVENT);
  int replacement = event_fd();
  assert(dup3(replacement, epfd, O_CLOEXEC) == epfd);
  enqueue(thread, PACKET, fd);
  assert(!pthread_join(thread, NULL) && call.result == 1);
  assert(call.event.events == EPOLLIN && call.event.data.u64 == SIGNAL_EVENT);

  struct epoll_event event;
  assert(epoll_wait(epfd, &event, 1, 0) == 0);
  assert(!close(epfd) && !close(replacement));

  int pipes[2];
  assert(!pipe2(pipes, O_CLOEXEC));
  struct sleeping_wait poll_call = {.fd = fd, .api = POLL};
  assert(!pthread_create(&thread, NULL, wait_once, &poll_call));
  wait_for_ppoll(&poll_call);
  assert(dup2(pipes[0], fd) == fd);
  assert(write(pipes[1], "q", 1) == 1);
  assert(!pthread_join(thread, NULL) && poll_call.result == 1);
  assert(poll_call.event.events == POLLIN);
  char byte;
  assert(read(fd, &byte, 1) == 1 && byte == 'q');
  assert(!close(fd) && !close(pipes[0]) && !close(pipes[1]));

  for (enum wait_api api = POLL; api <= SELECT; ++api) {
    struct sleeping_wait closed = {.fd = signal_fd(), .api = api};
    assert(!pthread_create(&thread, NULL, wait_once, &closed));
    wait_for_ppoll(&closed);
    assert(!close(closed.fd));
    assert(!pthread_join(thread, NULL));
    if (api == POLL) {
      assert(closed.result == 1 && closed.event.events == POLLNVAL);
    } else {
      assert(closed.result == -1 && closed.error == EBADF);
    }
  }
}

void sdk_guest_signalfd_ready_probe(void) {
  poll_select();
  modes_and_aliases();
  nested_watches();
  for (enum wait_api api = POLL; api <= EPOLL_NANOS; ++api) {
    separate_targets(api);
  }
  close_during_wait();
  temporary_masks();
  check_abandoned_waits();
  puts(
      "{\"stage\":\"guest-signalfd-ready\",\"poll_select\":true,"
      "\"epoll_modes\":true,\"nested\":true,\"separate_targets\":true,"
      "\"concurrent_close\":true,\"cancellation\":true,"
      "\"temporary_masks\":true,\"abandoned_waits\":true}");
  fflush(stdout);
}
