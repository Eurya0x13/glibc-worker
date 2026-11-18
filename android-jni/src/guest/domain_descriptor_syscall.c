#define _GNU_SOURCE
#include "domain_descriptor_syscall.h"
#include "guest-epoll.h"
#include "guest-fd-state.h"
#include "guest-ready.h"
#include "guest-signalfd.h"
#include "nonlocal-mask.h"
#include "signal-user-copy.h"
#include <errno.h>
#include <sys/personality.h>
#include <sys/syscall.h>

/* Before the embedded libc is identified, the owning syscall entry is safe.
 * Hook installation switches this pointer before any callback can recurse. */
#undef syscall
gw_kernel_syscall gw_guest_kernel_call = syscall;
#define syscall gw_guest_kernel_call

#define PTR(type, value) ((type *)(uintptr_t)(value))

struct mask_argument {
  const void *set;
  size_t size;
};

struct readiness_args {
  struct timespec timeout;
  void *user_timeout;
  const void *user_mask;
  size_t mask_size;
  int timed, timeval;
};

static long failure(int error, int *handled) {
  *handled = 1;
  errno = error;
  return -1;
}

static int import_timeout(struct readiness_args *args) {
  if (!args->user_timeout) {
    return 0;
  }

  args->timed = 1;
  if (!args->timeval) {
    return gw_signal_user_copy_in(&args->timeout, args->user_timeout,
                                  sizeof args->timeout);
  }

  struct timeval value;
  int error = gw_signal_user_copy_in(&value, args->user_timeout, sizeof value);
  if (error || value.tv_sec < 0 || value.tv_usec < 0) {
    return error ? error : EINVAL;
  }
  time_t seconds = value.tv_usec / 1000000;
  args->timeout = value.tv_sec > INT64_MAX - seconds
                      ? (struct timespec){INT64_MAX, 999999999}
                      : (struct timespec){value.tv_sec + seconds,
                                          value.tv_usec % 1000000 * 1000};
  return 0;
}

static int import_readiness(long number, const long a[6],
                            struct readiness_args *args) {
  long milliseconds = -1;
  switch (number) {
#ifdef SYS_poll
    case SYS_poll:
      milliseconds = (int)a[2];
      break;
#endif
    case SYS_ppoll:
      args->user_timeout = PTR(void, a[2]);
      args->user_mask = PTR(void, a[3]);
      args->mask_size = a[4];
      break;
#ifdef SYS_select
    case SYS_select:
      args->timeval = 1;
      args->user_timeout = PTR(void, a[4]);
      break;
#endif
    case SYS_pselect6: {
      args->user_timeout = PTR(void, a[4]);
      if (a[5]) {
        struct mask_argument mask;
        int error = gw_signal_user_copy_in(&mask, PTR(void, a[5]), sizeof mask);
        if (error) {
          return error;
        }
        args->user_mask = mask.set;
        args->mask_size = mask.size;
      }
      break;
    }
#ifdef SYS_epoll_wait
    case SYS_epoll_wait:
      milliseconds = (int)a[3];
      break;
#endif
    case SYS_epoll_pwait:
      milliseconds = (int)a[3];
      args->user_mask = PTR(void, a[4]);
      args->mask_size = a[5];
      break;
    case SYS_epoll_pwait2:
      args->user_timeout = PTR(void, a[3]);
      args->user_mask = PTR(void, a[4]);
      args->mask_size = a[5];
      break;
  }

  if (milliseconds >= 0) {
    args->timed = 1;
    args->timeout =
        (struct timespec){milliseconds / 1000, milliseconds % 1000 * 1000000};
  }
  return import_timeout(args);
}

static void copy_remaining(void *destination, int timeval,
                           const struct timespec *remaining) {
  int saved = errno;
  /* Like poll_select_finish, preserve the result on failed copyout and keep
   * the original timeout for the STICKY_TIMEOUTS personality.
   * https://github.com/torvalds/linux/blob/v6.12/fs/select.c */
  long personality = syscall(SYS_personality, 0xffffffffUL);
  if (personality < 0 || !(personality & STICKY_TIMEOUTS)) {
    if (timeval) {
      struct timeval value = {remaining->tv_sec, remaining->tv_nsec / 1000};
      gw_signal_user_copy_out(destination, &value, sizeof value);
    } else {
      gw_signal_user_copy_out(destination, remaining, sizeof *remaining);
    }
  }
  errno = saved;
}

static long raw_readiness(struct gw_signal_target *target, long number,
                          const long a[6],
                          const struct gw_libc_raw_descriptor_io *io,
                          int *handled) {
  if (!gw_guest_readiness_active()) {
    return 0;
  }

  struct readiness_args args = {0};
  int error = import_readiness(number, a, &args);
  if (error) {
    return failure(error, handled);
  }
  struct gw_domain_wait_mask transaction = {0};
  sigset_t set = {0};
  const sigset_t *mask = NULL;
  if (args.user_mask) {
    error =
        args.mask_size != sizeof(uint64_t)
            ? EINVAL
            : gw_signal_user_copy_in(&set, args.user_mask, sizeof(uint64_t));
    if (error || (error = io->mask_begin(&set, &transaction))) {
      return failure(error, handled);
    }
    mask = transaction.active ? &transaction.kernel : &set;
  }

  struct timespec remainder = args.timeout;
  struct timespec *timeout = args.timed ? &args.timeout : NULL;
  int copy_timeout =
      args.user_timeout && (args.timeout.tv_sec || args.timeout.tv_nsec);
  long result;
  switch (number) {
#ifdef SYS_poll
    case SYS_poll:
#endif
    case SYS_ppoll:
      result = gw_guest_poll(target, PTR(struct pollfd, a[0]), (unsigned)a[1],
                             timeout, mask, io->wait.poll, GW_IO_NOCANCEL,
                             copy_timeout ? &remainder : NULL, handled);
      break;
#ifdef SYS_select
    case SYS_select:
#endif
    case SYS_pselect6:
      result = gw_guest_select(target, (int)a[0], PTR(fd_set, a[1]),
                               PTR(fd_set, a[2]), PTR(fd_set, a[3]),
                               timeout ? &remainder : NULL, mask, io->wait.poll,
                               GW_IO_NOCANCEL, handled);
      break;
    default:
      *handled = 1;
      result = gw_guest_epoll_wait(
          target, (int)a[0], PTR(struct epoll_event, a[1]), (int)a[2], timeout,
          mask, io->wait.poll, GW_IO_NOCANCEL);
      copy_timeout = 0;
      break;
  }

  if (*handled && copy_timeout && (result >= 0 || errno == EINTR)) {
    copy_remaining(args.user_timeout, args.timeval, &remainder);
  }
  int saved = errno;
  error = io->mask_end(&transaction);
  errno = saved;
  return error ? failure(error, handled) : result;
}

static int managed_epoll(int fd) {
  int saved = errno;
  uint64_t physical = gw_nonlocal_mask_block();
  gw_guest_fds_lock();
  int managed = gw_guest_epoll_known(fd);
  gw_guest_fds_unlock();
  errno = saved;
  gw_nonlocal_mask_restore(physical);
  return managed;
}

long gw_guest_descriptor_syscall(struct gw_signal_target *target, long number,
                                 const long a[6],
                                 const struct gw_libc_raw_descriptor_io *io,
                                 int *handled) {
  switch (number) {
#ifdef SYS_signalfd
    case SYS_signalfd:
#endif
    case SYS_signalfd4:
      if ((size_t)a[2] != sizeof(uint64_t)) {
        return failure(EINVAL, handled);
      }
      return gw_guest_signalfd(target, (int)a[0], PTR(sigset_t, a[1]),
                               number == SYS_signalfd4 ? (int)a[3] : 0,
                               handled);
    case SYS_read:
      return gw_guest_signalfd_read(target, (int)a[0], PTR(void, a[1]), a[2],
                                    GW_IO_NOCANCEL, io->wait, handled);
    case SYS_readv:
      return gw_guest_signalfd_readv(target, (int)a[0], PTR(struct iovec, a[1]),
                                     (int)a[2], GW_IO_NOCANCEL, io->wait,
                                     handled);
    case SYS_close:
      return gw_guest_descriptor(target->registry, GW_DESCRIPTOR_CLOSE, a[0],
                                 -1, 0, GW_IO_NOCANCEL, handled);
#ifdef SYS_dup2
    case SYS_dup2:
      return gw_guest_descriptor(target->registry, GW_DESCRIPTOR_DUP2, a[0],
                                 a[1], 0, GW_IO_NOCANCEL, handled);
#endif
    case SYS_dup3:
      return gw_guest_descriptor(target->registry, GW_DESCRIPTOR_DUP3, a[0],
                                 a[1], a[2], GW_IO_NOCANCEL, handled);
    case SYS_epoll_ctl: {
      int saved = errno;
      int result = gw_guest_epoll_ctl(target->registry, a[0], a[1], a[2],
                                      PTR(struct epoll_event, a[3]));
      *handled = 1;
      errno = result < 0 ? -result : saved;
      return result < 0 ? -1 : result;
    }
#ifdef SYS_poll
    case SYS_poll:
#endif
#ifdef SYS_select
    case SYS_select:
#endif
    case SYS_ppoll:
    case SYS_pselect6:
      return raw_readiness(target, number, a, io, handled);
#ifdef SYS_epoll_wait
    case SYS_epoll_wait:
#endif
    case SYS_epoll_pwait:
    case SYS_epoll_pwait2:
      return gw_guest_readiness_active() && managed_epoll(a[0])
                 ? raw_readiness(target, number, a, io, handled)
                 : 0;
    default:
      return 0;
  }
}
