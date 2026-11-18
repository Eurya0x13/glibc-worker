/* Optional logical descriptor callbacks for the owning GNU libc.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef GW_LIBC_SIGNALFD_H
#define GW_LIBC_SIGNALFD_H
#include "signal-consume-hook.h"
#include <poll.h>
#include <sys/epoll.h>
#include <sys/select.h>
#include <sys/uio.h>

typedef int (*gw_libc_poll_wait)(struct pollfd *, nfds_t, struct timespec *,
                                 const void *);

#define GW_LIBC_SIGNALFD_VERSION 4

enum gw_libc_io_policy {
  GW_IO_NOCANCEL,
  GW_IO_CANCELLABLE,
};

enum gw_libc_descriptor_op {
  GW_DESCRIPTOR_CLOSE,
  GW_DESCRIPTOR_DUP2,
  GW_DESCRIPTOR_DUP3,
};

struct gw_domain_wait_mask {
  sigset_t kernel;
  uint64_t physical, logical;
  int active;
};

struct gw_libc_fd_wait {
  gw_libc_wait_event futex;
  gw_libc_poll_wait poll;
};

struct gw_libc_raw_descriptor_io {
  int (*mask_begin)(const sigset_t *, struct gw_domain_wait_mask *);
  int (*mask_end)(struct gw_domain_wait_mask *);
  struct gw_libc_fd_wait wait;
};

struct gw_libc_signalfd_hook {
  size_t size;
  int (*open)(int, const sigset_t *, int, int *handled);
  ssize_t (*read)(int, void *, size_t, enum gw_libc_io_policy,
                  struct gw_libc_fd_wait, int *handled);
  ssize_t (*readv)(int, const struct iovec *, int, enum gw_libc_io_policy,
                   struct gw_libc_fd_wait, int *handled);
  int (*descriptor)(enum gw_libc_descriptor_op, int, int, int,
                    enum gw_libc_io_policy, int *);
  int (*poll)(struct pollfd *, nfds_t, const struct timespec *,
              const sigset_t *, gw_libc_poll_wait, int *);
  int (*select)(int, fd_set *, fd_set *, fd_set *, struct timespec *,
                const sigset_t *, gw_libc_poll_wait, int *);
  int (*epoll_ctl)(int, int, int, const struct epoll_event *, int *);
  int (*epoll_wait)(int, struct epoll_event *, int, const struct timespec *,
                    const sigset_t *, gw_libc_poll_wait, int *);
  void (*unwind)(uintptr_t, uintptr_t);
  void (*restore)(uintptr_t, uintptr_t, size_t);
  long (*raw)(long, const long[6], const struct gw_libc_raw_descriptor_io *,
              int *);
};

int __gw_domain_signalfd_hook(unsigned, const struct gw_libc_signalfd_hook *);
#endif
