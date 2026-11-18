#define _GNU_SOURCE
#include "host-io.h"
#include "raw_syscall.h"
#include "signal_channel.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>

static void check_futex(struct async_carrier *carrier, int phase) {
  struct timespec deadline;
  assert(!clock_gettime(CLOCK_MONOTONIC, &deadline));
  deadline.tv_sec += 3;
  atomic_store(&carrier->io_word, 0);
  errno = EDOM;
  atomic_store_explicit(&carrier->phase, phase, memory_order_release);

  int result = phase == PHASE_RAW_FUTEX
                   ? syscall(SYS_futex, &carrier->io_word,
                             FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG, 0,
                             &deadline, NULL, FUTEX_BITSET_MATCH_ANY)
                   : gw_host_futex_wait(&carrier->io_word, 0, &deadline);
  if (phase == PHASE_HOST_FUTEX) {
    assert(!result && errno == EDOM && atomic_load(&carrier->io_word) == 1);
  } else {
    assert(result == -1 && errno == EINTR);
  }
}

static ssize_t blocked_io(int phase, int fd, char *byte, FILE *stream) {
  struct iovec iov = {.iov_base = byte, .iov_len = 1};
  switch (phase) {
    case PHASE_READ:
    case PHASE_TIMED_RECV:
      return read(fd, byte, 1);
    case PHASE_READV:
      return readv(fd, &iov, 1);
    case PHASE_FREAD:
      return fread(byte, 1, 1, stream);
    case PHASE_SYSCALL_READ:
      return syscall(SYS_read, fd, byte, 1);
    case PHASE_INLINE_READ:
      return gw_raw3(SYS_read, fd, (long)byte, 1);
    case PHASE_WRITE:
      return write(fd, byte, 1);
    case PHASE_WRITEV:
      return writev(fd, &iov, 1);
  }
  abort();
}

static void fill_pipe(struct async_carrier *carrier) {
  carrier->pipe_size = fcntl(carrier->io_fds[1], F_GETPIPE_SZ);
  assert(carrier->pipe_size > 0);

  char *pending = calloc((size_t)carrier->pipe_size, 1);
  assert(pending);
  assert(write(carrier->io_fds[1], pending, carrier->pipe_size) ==
         carrier->pipe_size);
  free(pending);
}

static void check_io(struct async_carrier *carrier, int phase) {
  int writing = phase == PHASE_WRITE || phase == PHASE_WRITEV;
  if (phase == PHASE_TIMED_RECV) {
    assert(!socketpair(AF_UNIX, SOCK_DGRAM, 0, carrier->io_fds));
    struct timeval timeout = {.tv_sec = 3};
    assert(!setsockopt(carrier->io_fds[0], SOL_SOCKET, SO_RCVTIMEO, &timeout,
                       sizeof timeout));
  } else {
    assert(!pipe(carrier->io_fds));
  }
  if (writing) {
    fill_pipe(carrier);
  }

  int fd = carrier->io_fds[writing];
  FILE *stream = NULL;
  if (phase == PHASE_FREAD) {
    /* GNU stdio's ordinary buffered file read enters hidden __read. On
     * Android exercise the actual owning-libc FILE backend too. */
    stream = fdopen(fd, "r");
    assert(stream);
  }

  char byte = writing ? 'w' : '\0';
  errno = EDOM;
  atomic_store_explicit(&carrier->phase, phase, memory_order_release);
  ssize_t result = blocked_io(phase, fd, &byte, stream);
  if (phase == PHASE_TIMED_RECV) {
    assert(result == -1 && errno == EINTR);
  } else {
    assert(result == 1 && byte == 'w' && errno == EDOM);
  }
  assert(!(stream ? fclose(stream) : close(fd)));
}

void check_host_io(struct async_carrier *carrier) {
  for (int phase = PHASE_READ; phase <= PHASE_VISIBLE_FUTEX; ++phase) {
    if (phase < PHASE_RAW_FUTEX) {
      check_io(carrier, phase);
    } else {
      check_futex(carrier, phase);
    }
    while (atomic_load_explicit(&carrier->io_done, memory_order_acquire) !=
           phase) {
      sched_yield();
    }
  }
}

long host_io_call_number(int phase) {
  switch (phase) {
    case PHASE_READV:
      return SYS_readv;
    case PHASE_WRITE:
      return SYS_write;
    case PHASE_WRITEV:
      return SYS_writev;
    case PHASE_RAW_FUTEX:
    case PHASE_HOST_FUTEX:
    case PHASE_VISIBLE_FUTEX:
      return SYS_futex;
    default:
      return SYS_read;
  }
}

static void drain_pipe(int descriptor, int pending) {
  char bytes[4096];
  while (pending) {
    size_t space = pending < (int)sizeof bytes ? (size_t)pending : sizeof bytes;
    ssize_t count = read(descriptor, bytes, space);
    assert(count > 0);
    for (ssize_t i = 0; i < count; ++i) {
      assert(!bytes[i]);
    }
    pending -= (int)count;
  }
  assert(read(descriptor, bytes, 1) == 1 && bytes[0] == 'w');
  assert(!close(descriptor));
}

void host_io_unblock(struct async_carrier *carrier, int phase) {
  switch (phase) {
    case PHASE_WRITE:
    case PHASE_WRITEV:
      drain_pipe(carrier->io_fds[0], carrier->pipe_size);
      break;
    case PHASE_TIMED_RECV:
      assert(!close(carrier->io_fds[1]));
      break;
    case PHASE_HOST_FUTEX:
      atomic_store(&carrier->io_word, 1);
      assert(syscall(SYS_futex, &carrier->io_word, FUTEX_WAKE_PRIVATE, 1, NULL,
                     NULL, 0) >= 0);
      break;
    case PHASE_RAW_FUTEX:
    case PHASE_VISIBLE_FUTEX:
      break;
    default:
      assert(write(carrier->io_fds[1], "w", 1) == 1);
      assert(!close(carrier->io_fds[1]));
  }

  atomic_store_explicit(&carrier->io_done, phase, memory_order_release);
}
