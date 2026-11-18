#define _GNU_SOURCE
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

enum { PACKET = 56, OTHER = 57 };

static sigset_t one(int number) {
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, number);
  return mask;
}

static void send(pthread_t target, int number, int value) {
  assert(!pthread_sigqueue(target, number, (union sigval){.sival_int = value}));
}

static void check_packet(const struct signalfd_siginfo *packet, int number,
                         int value) {
  assert(packet->ssi_signo == (uint32_t)number && packet->ssi_code == SI_QUEUE);
  assert(packet->ssi_pid == (uint32_t)getpid() && packet->ssi_uid == getuid());
  assert(packet->ssi_int == value && packet->ssi_ptr == (uint32_t)value);
}

static void aliases_and_copyout(void) {
  sigset_t set = one(PACKET);
  int fd = signalfd(-1, &set, SFD_NONBLOCK | SFD_CLOEXEC);
  assert(fd >= 0 && (fcntl(fd, F_GETFL) & O_NONBLOCK));
  assert(fcntl(fd, F_GETFD) == FD_CLOEXEC);
  int alias = dup(fd);
  assert(alias >= 0 && fcntl(alias, F_GETFD) == 0);

  struct signalfd_siginfo packets[3];
  assert(read(fd, packets, sizeof packets) == -1 && errno == EAGAIN);
  send(pthread_self(), PACKET, 18);
  send(pthread_self(), PACKET, 23);
  assert(read(fd, packets, sizeof packets[0] - 1) == -1 && errno == EINVAL);
  errno = EDOM;
  assert(read(alias, packets, sizeof packets) == 2 * sizeof packets[0]);
  assert(errno == EDOM);
  check_packet(&packets[0], PACKET, 18);
  check_packet(&packets[1], PACKET, 23);

  size_t page = (size_t)sysconf(_SC_PAGESIZE);
  void *bad = mmap(NULL, page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(bad != MAP_FAILED);
  assert(signalfd(alias, bad, 0) == -1 && errno == EFAULT);
  send(pthread_self(), PACKET, 81);
  assert(read(alias, bad, sizeof packets[0]) == -1 && errno == EFAULT);
  assert(read(fd, packets, sizeof packets) == -1 && errno == EAGAIN);
  assert(!munmap(bad, page));

  set = one(OTHER);
  assert(signalfd(alias, &set, SFD_CLOEXEC) == alias);
  assert(fcntl(alias, F_GETFD) == 0 && (fcntl(alias, F_GETFL) & O_NONBLOCK));
  assert(!close(fd));
  send(pthread_self(), OTHER, 57);
  assert(read(alias, packets, sizeof packets) == sizeof packets[0]);
  check_packet(packets, OTHER, 57);

  int native[2];
  assert(!pipe2(native, O_CLOEXEC));
  assert(dup2(native[0], alias) == alias);
  assert(write(native[1], "x", 1) == 1);
  char byte;
  assert(read(alias, &byte, 1) == 1 && byte == 'x');
  assert(!close(alias) && !close(native[0]) && !close(native[1]));
}

static void vector_reads(void) {
  uint64_t empty = 0;
  int native = syscall(SYS_signalfd4, -1, &empty, sizeof empty, SFD_NONBLOCK);
  assert(native >= 0);
  struct signalfd_siginfo packet;
  const size_t half = sizeof packet / 2;
  struct iovec split[] = {{&packet, half}, {(char *)&packet + half, half}};
  assert(syscall(SYS_readv, native, split, 2) == -1);
  int legacy = errno == EINVAL;
  assert(legacy || errno == EAGAIN);
  assert(!close(native));

  sigset_t mask = one(PACKET);
  int fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
  assert(fd >= 0);
  send(pthread_self(), PACKET, 101);
  ssize_t received = readv(fd, split, 2);
  if (legacy) {
    assert(received == -1 && errno == EINVAL);
    struct iovec full = {&packet, sizeof packet};
    received = readv(fd, &full, 1);
  }
  assert(received == sizeof packet);
  check_packet(&packet, PACKET, 101);

  /* The first record overwrites the input vector array. The second still
   * goes to its originally imported buffer, just as it does in the kernel. */
  union {
    struct iovec input[2];
    struct signalfd_siginfo output;
  } first;

  first.input[0] = (struct iovec){&first.output, sizeof first.output};
  first.input[1] = (struct iovec){&packet, sizeof packet};
  send(pthread_self(), PACKET, 111);
  send(pthread_self(), PACKET, 117);
  assert(readv(fd, first.input, 2) == 2 * sizeof packet);
  check_packet(&first.output, PACKET, 111);
  check_packet(&packet, PACKET, 117);

  size_t page = (size_t)sysconf(_SC_PAGESIZE);
  void *bad = mmap(NULL, page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(bad != MAP_FAILED);
  struct iovec partial[] = {{&packet, sizeof packet}, {bad, sizeof packet}};
  send(pthread_self(), PACKET, 122);
  send(pthread_self(), PACKET, 126);
  assert(readv(fd, partial, 2) == sizeof packet);
  check_packet(&packet, PACKET, 122);
  assert(read(fd, &packet, sizeof packet) == -1 && errno == EAGAIN);
  assert(!munmap(bad, page));
  assert(!close(fd));
}

struct blocked_read {
  int fd, vectored;
  _Atomic int tid;
  struct signalfd_siginfo packet;
  ssize_t result;
  int error;
};

static void *reader(void *argument) {
  struct blocked_read *call = argument;
  atomic_store_explicit(&call->tid, (int)syscall(SYS_gettid),
                        memory_order_release);
  struct iovec output = {&call->packet, sizeof call->packet};
  call->result = call->vectored
                     ? readv(call->fd, &output, 1)
                     : read(call->fd, &call->packet, sizeof call->packet);
  call->error = errno;
  return NULL;
}

static void wait_for_syscall(_Atomic int *thread_id, long expected) {
  struct timespec start, now;
  assert(!clock_gettime(CLOCK_MONOTONIC, &start));
  for (;;) {
    int tid = atomic_load_explicit(thread_id, memory_order_acquire);
    if (tid) {
      char name[80];
      assert(snprintf(name, sizeof name, "/proc/self/task/%d/syscall", tid) >
             0);
      FILE *status = fopen(name, "r");
      assert(status);
      long number;
      int waiting = fscanf(status, "%ld", &number) == 1 && number == expected;
      assert(!fclose(status));
      if (waiting) {
        return;
      }
    }
    assert(!clock_gettime(CLOCK_MONOTONIC, &now));
    assert(now.tv_sec - start.tv_sec < 5);
    sched_yield();
  }
}

static void wait_for_read(struct blocked_read *call) {
  wait_for_syscall(&call->tid, SYS_futex);
}

static void mask_update(void) {
  sigset_t mask = one(PACKET);
  struct blocked_read call = {.fd = signalfd(-1, &mask, SFD_CLOEXEC),
                              .vectored = 1};
  assert(call.fd >= 0 && !(fcntl(call.fd, F_GETFL) & O_NONBLOCK));
  pthread_t thread;
  assert(!pthread_create(&thread, NULL, reader, &call));
  wait_for_read(&call);
  send(thread, OTHER, 66);

  int alias = dup(call.fd);
  assert(alias >= 0);
  assert(!close(call.fd));
  mask = one(OTHER);
  assert(signalfd(alias, &mask, SFD_NONBLOCK) == alias);
  assert(!pthread_join(thread, NULL));
  assert(call.result == sizeof call.packet);
  check_packet(&call.packet, OTHER, 66);
  assert(!close(alias));
}

static _Atomic int interrupted;

static void interrupt(int number) {
  assert(number == SIGURG);
  atomic_fetch_add_explicit(&interrupted, 1, memory_order_release);
}

static void interrupt_read(int restart) {
  interrupted = 0;
  struct sigaction action = {.sa_handler = interrupt, .sa_flags = restart};
  struct sigaction previous;
  sigemptyset(&action.sa_mask);
  assert(!sigaction(SIGURG, &action, &previous));
  sigset_t mask = one(SIGURG), original;
  assert(!pthread_sigmask(SIG_UNBLOCK, &mask, &original));
  mask = one(PACKET);
  struct blocked_read call = {.fd = signalfd(-1, &mask, SFD_CLOEXEC)};
  assert(call.fd >= 0);

  pthread_t thread;
  assert(!pthread_create(&thread, NULL, reader, &call));
  wait_for_read(&call);
  assert(!pthread_kill(thread, SIGURG));
  if (restart) {
    while (!atomic_load_explicit(&interrupted, memory_order_acquire)) {
      sched_yield();
    }
    wait_for_read(&call);
    send(thread, PACKET, 91);
  }
  assert(!pthread_join(thread, NULL) && interrupted == 1);
  if (restart) {
    assert(call.result == sizeof call.packet);
    check_packet(&call.packet, PACKET, 91);
  } else {
    assert(call.result == -1 && call.error == EINTR);
  }
  assert(!close(call.fd));
  assert(!pthread_sigmask(SIG_SETMASK, &original, NULL));
  assert(!sigaction(SIGURG, &previous, NULL));
}

static _Atomic int hold_stage;

static void hold_reader(int number) {
  assert(number == SIGURG);
  atomic_store_explicit(&hold_stage, 1, memory_order_release);
  while (atomic_load_explicit(&hold_stage, memory_order_acquire) != 2) {
    sched_yield();
  }
}

static void close_read_description(void) {
  struct sigaction action = {.sa_handler = hold_reader, .sa_flags = SA_RESTART};
  struct sigaction previous;
  sigemptyset(&action.sa_mask);
  assert(!sigaction(SIGURG, &action, &previous));
  sigset_t mask = one(SIGURG), inherited;
  assert(!pthread_sigmask(SIG_UNBLOCK, &mask, &inherited));

  mask = one(PACKET);
  struct blocked_read call = {.fd = signalfd(-1, &mask, SFD_CLOEXEC)};
  assert(call.fd >= 0);
  pthread_t thread;
  assert(!pthread_create(&thread, NULL, reader, &call));
  wait_for_read(&call);
  hold_stage = 0;
  assert(!pthread_kill(thread, SIGURG));
  while (atomic_load_explicit(&hold_stage, memory_order_acquire) != 1) {
    sched_yield();
  }

  /* Pin the reader in its handler across the mask change and final user
   * close. The restarted read must use the original OFD's updated mask. */
  mask = one(OTHER);
  assert(signalfd(call.fd, &mask, 0) == call.fd);
  int native[2];
  assert(!pipe2(native, O_CLOEXEC));
  assert(dup2(native[0], call.fd) == call.fd);
  assert(write(native[1], "n", 1) == 1);
  send(thread, OTHER, 123);
  atomic_store_explicit(&hold_stage, 2, memory_order_release);

  assert(!pthread_join(thread, NULL) && call.result == sizeof call.packet);
  check_packet(&call.packet, OTHER, 123);
  char byte;
  assert(read(call.fd, &byte, 1) == 1 && byte == 'n');
  assert(!close(call.fd) && !close(native[0]) && !close(native[1]));
  assert(!pthread_sigmask(SIG_SETMASK, &inherited, NULL));
  assert(!sigaction(SIGURG, &previous, NULL));
}

static void cancel_read(void) {
  sigset_t mask = one(PACKET);
  struct blocked_read call = {.fd = signalfd(-1, &mask, SFD_CLOEXEC)};
  assert(call.fd >= 0);
  pthread_t thread;
  assert(!pthread_create(&thread, NULL, reader, &call));
  wait_for_read(&call);
  assert(!pthread_cancel(thread));
  void *result;
  assert(!pthread_join(thread, &result) && result == PTHREAD_CANCELED);
  assert(!close(call.fd));
}

static void stdio_reads(void) {
  sigset_t mask = one(PACKET);
  int fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
  assert(fd >= 0);
  FILE *file = fdopen(fd, "r");
  assert(file && fileno(file) == fd);
  char buffer[4 * sizeof(struct signalfd_siginfo)];
  assert(!setvbuf(file, buffer, _IOFBF, sizeof buffer));

  send(pthread_self(), PACKET, 301);
  send(pthread_self(), PACKET, 302);
  struct signalfd_siginfo packet;
  assert(fread(&packet, sizeof packet, 1, file) == 1);
  check_packet(&packet, PACKET, 301);
  struct pollfd watched = {.fd = fd, .events = POLLIN};
  assert(poll(&watched, 1, 0) == 0);
  assert(fread(&packet, sizeof packet, 1, file) == 1);
  check_packet(&packet, PACKET, 302);

  assert(fread(&packet, sizeof packet, 1, file) == 0);
  assert(ferror(file) && !feof(file) && errno == EAGAIN);
  clearerr(file);
  send(pthread_self(), PACKET, 303);
  assert(fread(&packet, sizeof packet, 1, file) == 1);
  check_packet(&packet, PACKET, 303);
  assert(!fclose(file));

  fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
  assert(fd >= 0);
  file = fdopen(fd, "r");
  assert(file && !setvbuf(file, NULL, _IONBF, 0));
  send(pthread_self(), PACKET, 304);
  assert(fgetc(file) == EOF && ferror(file) && errno == EINVAL);
  clearerr(file);
  assert(fread(&packet, sizeof packet, 1, file) == 1);
  check_packet(&packet, PACKET, 304);
  assert(!fclose(file));
}

static void *nocancel_file_read(void *argument) {
  struct blocked_read *call = argument;
  FILE *file = fdopen(call->fd, "r");
  assert(file);
  /* fdopen does not parse fopen's 'c' flag. Exercise the reviewed native
   * FILE read branch, as in the Host stdio control.
   * https://github.com/bminor/glibc/blob/glibc-2.42/libio/libio.h
   * https://github.com/bminor/glibc/blob/glibc-2.42/libio/fileops.c
   */
  file->_flags2 |= 2;

  assert(!pthread_cancel(pthread_self()));
  atomic_store_explicit(&call->tid, (int)syscall(SYS_gettid),
                        memory_order_release);
  assert(fread(&call->packet, sizeof call->packet, 1, file) == 1);
  assert(!fclose(file));
  call->result = 1;
  pthread_testcancel();
  assert(0);
  return NULL;
}

static void stdio_nocancel(void) {
  sigset_t mask = one(PACKET);
  struct blocked_read call = {.fd = signalfd(-1, &mask, SFD_CLOEXEC)};
  assert(call.fd >= 0);
  pthread_t thread;
  assert(!pthread_create(&thread, NULL, nocancel_file_read, &call));
  wait_for_read(&call);
  send(thread, PACKET, 311);

  void *result;
  assert(!pthread_join(thread, &result) && result == PTHREAD_CANCELED);
  assert(call.result == 1);
  check_packet(&call.packet, PACKET, 311);
}

struct closing_poll {
  _Atomic int tid;
  struct pollfd watch;
  int result;
};

static void *poll_until_closed(void *argument) {
  struct closing_poll *call = argument;
  atomic_store_explicit(&call->tid, (int)syscall(SYS_gettid),
                        memory_order_release);
  call->result = poll(&call->watch, 1, -1);
  return NULL;
}

static void close_notifies_waiter(FILE *file, int fd,
                                  void (*close_nostatus)(int)) {
  struct closing_poll call = {.watch = {.fd = fd, .events = POLLIN}};
  pthread_t thread;
  assert(!pthread_create(&thread, NULL, poll_until_closed, &call));
  wait_for_syscall(&call.tid, SYS_ppoll);

  if (file) {
    assert(!fclose(file));
  } else {
    errno = EDOM;
    close_nostatus(fd);
    assert(errno == EDOM);
  }

  struct timespec deadline;
  assert(!clock_gettime(CLOCK_REALTIME, &deadline));
  deadline.tv_sec += 3;
  assert(!pthread_timedjoin_np(thread, NULL, &deadline));
  assert(call.result == 1 && call.watch.revents == POLLNVAL);
}

static void stdio_close(void) {
  sigset_t mask = one(PACKET);
  int fd = signalfd(-1, &mask, SFD_CLOEXEC);
  assert(fd >= 0);
  FILE *file = fdopen(fd, "r");
  assert(file);
  close_notifies_waiter(file, fd, NULL);

  void (*close_nostatus)(int) =
      dlvsym(RTLD_DEFAULT, "__close_nocancel_nostatus", "GLIBC_PRIVATE");
  assert(close_nostatus);
  fd = signalfd(-1, &mask, SFD_CLOEXEC);
  assert(fd >= 0);
  close_notifies_waiter(NULL, fd, close_nostatus);
}

void sdk_guest_signalfd_probe(void) {
  sigset_t mask = one(PACKET), inherited;
  sigaddset(&mask, OTHER);
  assert(!pthread_sigmask(SIG_BLOCK, &mask, &inherited));
  aliases_and_copyout();
  vector_reads();
  mask_update();
  interrupt_read(0);
  interrupt_read(SA_RESTART);
  cancel_read();
  close_read_description();
  stdio_reads();
  stdio_nocancel();
  stdio_close();
  extern void sdk_guest_signalfd_ready_probe(void);
  sdk_guest_signalfd_ready_probe();
  assert(!pthread_sigmask(SIG_SETMASK, &inherited, NULL));
  puts(
      "{\"stage\":\"guest-signalfd\",\"aliases\":true,\"copyout\":true,"
      "\"vectors\":true,\"mask_update\":true,\"interruption\":true,"
      "\"restart\":true,"
      "\"cancellation\":true,\"stdio\":true,\"stdio_nocancel\":true,"
      "\"stdio_close\":true}");
  fflush(stdout);
}
