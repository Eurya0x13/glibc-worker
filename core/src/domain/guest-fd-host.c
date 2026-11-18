#define _GNU_SOURCE
#include "guest-fd-host.h"
#include "guest-epoll.h"
#include "guest-fd-lease.h"
#include "guest-fd-state.h"
#include "guest-ready.h"
#include "guest-signalfd.h"
#include "nonlocal-mask.h"
#include "nonlocal.h"
#include "signal-router.h"
#include "signal-select.h"
#include <errno.h>
#include <limits.h>
#include <linux/futex.h>
#include <pthread.h>

const void *gw_guest_fd_scope(void) {
  return gw_nonlocal_scope_current();
}

static void retire_reader(struct gw_guest_fd_lease *loan) {
  struct gw_guest_fd_state *state = gw_guest_fds_state();
  struct gw_signal_target *reader = loan->storage;
  gw_signal_target_poll_disarm(reader);
  if (reader->wait_fd >= 0) {
    syscall(SYS_close, reader->wait_fd);
  }
  if (reader->selection) {
    gw_signal_target_unpin(reader->selection);
  }
  gw_signal_receiver_close(&reader->receiver);
  gw_signal_receiver_destroy(&reader->receiver);

  gw_guest_fds_lock();
  --state->host_calls;
  gw_guest_fds_unlock();
}

/* A Host-only TID sees the process-shared Guest queue. An adopted TID also
 * sees its own directed signals. In either case the wait itself belongs to
 * Host, including cancellation and which handler can interrupt it. */
static int open_reader(struct gw_guest_fd_state *state,
                       struct gw_guest_fd_lease **out, uintptr_t stack) {
  int error = gw_guest_fd_lease_open(out, NULL, -1, stack);
  if (error) {
    return error;
  }

  struct gw_guest_fd_lease *loan = *out;
  struct gw_signal_target *reader =
      gw_guest_fd_lease_reserve(loan, sizeof *reader);
  if (!reader) {
    return -errno;
  }
  int tid = (int)syscall(SYS_gettid);
  error = gw_signal_receiver_open(&state->targets->inbox, &reader->receiver,
                                  UINT64_MAX);
  if (error) {
    return -error;
  }

  reader->registry = state->targets;
  reader->tid = tid;
  reader->wait_fd = -1;
  reader->selection = gw_signal_targets_pin_tid(state->targets, tid);
  loan->target = reader;
  loan->retire = retire_reader;
  ++state->host_calls;
  return 1;
}

static int begin(struct gw_guest_fd_lease **loan, int fd, int scan_set,
                 int *handled) {
  *handled = 0;
  struct gw_guest_fd_state *state = gw_guest_fds_state();
  if (!state->targets) {
    return 0;
  }

  int saved = errno;
  uint64_t physical = gw_nonlocal_mask_block();
  gw_guest_fds_lock();
  int status = 0;
  if (!state->closed &&
      (scan_set || gw_guest_signalfd_cookie(fd) || gw_guest_epoll_known(fd))) {
    status = state->host_calls == UINT_MAX
                 ? -EOVERFLOW
                 : open_reader(state, loan, (uintptr_t)loan);
  }
  gw_guest_fds_unlock();
  *handled = status < 0;
  errno = status < 0 ? -status : saved;
  gw_nonlocal_mask_restore(physical);
  return status < 0 ? -1 : status;
}

static void abandon_wait(void **record) {
  if (*record) {
    gw_signal_native_wait_cancel_end(*record);
    gw_signal_mask_wait_end(*record, -1, ECANCELED);
  }
}

static int wait_io(long call, long a, long b, long c, int cancellable) {
  if (!signal_router_enabled()) {
#ifndef __ANDROID__
    int previous;
    if (cancellable) {
      pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, &previous);
    }
#endif
    int result = syscall(call, a, b, c, 0L, sizeof(uint64_t), 0L);
#ifndef __ANDROID__
    if (cancellable) {
      pthread_setcanceltype(previous, NULL);
    }
#endif
    return result;
  }

  void *record __attribute__((cleanup(abandon_wait))) =
      gw_signal_mask_wait_current_begin();
  if (!record) {
    return -1;
  }

  int result, error;
  do {
    if (cancellable) {
      gw_signal_native_wait_cancel_begin(record);
    }
    result = gw_signal_wait_result(gw_signal_native_wait_syscall(
        record, call, a, b, c, 0, sizeof(uint64_t), 0));
    error = errno;
    gw_signal_native_wait_cancel_end(record);
  } while (gw_signal_native_read_retry(record, result, error, cancellable));
  gw_signal_mask_wait_end(record, result, error);
  record = NULL;
  return result;
}

static int futex_wait(void *word, uint32_t sequence,
                      const struct timespec *deadline) {
  (void)deadline;
  int result = wait_io(SYS_futex, (long)word, FUTEX_WAIT_PRIVATE, sequence, 0);
  return result < 0 ? -errno : 0;
}

static int cancellable_wait(void *word, uint32_t sequence,
                            const struct timespec *deadline) {
  (void)deadline;
  int result = wait_io(SYS_futex, (long)word, FUTEX_WAIT_PRIVATE, sequence, 1);
  return result < 0 ? -errno : 0;
}

static int poll_wait(struct pollfd *fds, nfds_t count, struct timespec *timeout,
                     const void *mask) {
  (void)mask;
  return wait_io(SYS_ppoll, (long)fds, count, (long)timeout, 0);
}

static int cancellable_poll(struct pollfd *fds, nfds_t count,
                            struct timespec *timeout, const void *mask) {
  (void)mask;
  return wait_io(SYS_ppoll, (long)fds, count, (long)timeout, 1);
}

static const struct gw_libc_fd_wait cancel_io = {cancellable_wait,
                                                 cancellable_poll};
static const struct gw_libc_fd_wait nocancel_io = {futex_wait, poll_wait};

ssize_t gw_host_guest_read(int fd, void *out, size_t count,
                           enum gw_libc_io_policy policy, int *handled) {
  struct gw_guest_fd_lease *loan
      __attribute__((cleanup(gw_guest_fd_lease_drop))) = NULL;
  int status = begin(&loan, fd, 0, handled);
  if (status <= 0) {
    return status;
  }

  struct gw_libc_fd_wait wait =
      policy == GW_IO_CANCELLABLE ? cancel_io : nocancel_io;
  return gw_guest_signalfd_read(loan->target, fd, out, count, policy, wait,
                                handled);
}

ssize_t gw_host_guest_readv(int fd, const struct iovec *out, int count,
                            int *handled) {
  struct gw_guest_fd_lease *loan
      __attribute__((cleanup(gw_guest_fd_lease_drop))) = NULL;
  int status = begin(&loan, fd, 0, handled);
  if (status <= 0) {
    return status;
  }
  return gw_guest_signalfd_readv(loan->target, fd, out, count,
                                 GW_IO_CANCELLABLE, cancel_io, handled);
}

int gw_host_guest_poll(struct pollfd *fds, nfds_t count,
                       const struct timespec *timeout, const sigset_t *mask,
                       gw_libc_poll_wait wait, int *handled) {
  *handled = 0;
  if (!gw_guest_readiness_active()) {
    return 0;
  }
  struct gw_guest_fd_lease *loan
      __attribute__((cleanup(gw_guest_fd_lease_drop))) = NULL;
  int status = begin(&loan, -1, 1, handled);
  return status <= 0 ? status
                     : gw_guest_poll(loan->target, fds, count, timeout, mask,
                                     wait, GW_IO_CANCELLABLE, NULL, handled);
}

int gw_host_guest_select(int n, fd_set *read, fd_set *write, fd_set *except,
                         struct timespec *timeout, const sigset_t *mask,
                         gw_libc_poll_wait wait, int *handled) {
  *handled = 0;
  if (!gw_guest_readiness_active()) {
    return 0;
  }
  struct gw_guest_fd_lease *loan
      __attribute__((cleanup(gw_guest_fd_lease_drop))) = NULL;
  int status = begin(&loan, -1, 1, handled);
  return status <= 0
             ? status
             : gw_guest_select(loan->target, n, read, write, except, timeout,
                               mask, wait, GW_IO_CANCELLABLE, handled);
}

int gw_host_guest_epoll_wait(int fd, struct epoll_event *events, int count,
                             const struct timespec *timeout,
                             const sigset_t *mask, gw_libc_poll_wait wait,
                             int *handled) {
  struct gw_guest_fd_lease *loan
      __attribute__((cleanup(gw_guest_fd_lease_drop))) = NULL;
  int status = begin(&loan, fd, 0, handled);
  if (status <= 0) {
    return status;
  }
  *handled = 1;
  return gw_guest_epoll_wait(loan->target, fd, events, count, timeout, mask,
                             wait, GW_IO_CANCELLABLE);
}

int gw_host_guest_epoll_ctl(int epfd, int op, int fd,
                            const struct epoll_event *event, int *handled) {
  struct gw_guest_fd_state *state = gw_guest_fds_state();
  *handled = state->targets != NULL;
  if (!*handled) {
    return 0;
  }

  int saved = errno;
  int status = gw_guest_epoll_ctl(state->targets, epfd, op, fd, event);
  errno = status < 0 ? -status : saved;
  return status < 0 ? -1 : status;
}

int gw_host_guest_descriptor(enum gw_libc_descriptor_op op, int fd, int other,
                             int flags, enum gw_libc_io_policy policy,
                             int *handled) {
  *handled = 0;
  struct gw_guest_fd_state *state = gw_guest_fds_state();
  if (!state->targets) {
    return 0;
  }
  return gw_guest_descriptor(state->targets, op, fd, other, flags, policy,
                             handled);
}
