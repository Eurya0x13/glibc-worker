#ifndef GW_GUEST_FD_HOST_H
#define GW_GUEST_FD_HOST_H
#include "signalfd-hook.h"

ssize_t gw_host_guest_read(int, void *, size_t, enum gw_libc_io_policy, int *);
ssize_t gw_host_guest_readv(int, const struct iovec *, int, int *);
int gw_host_guest_poll(struct pollfd *, nfds_t, const struct timespec *,
                       const sigset_t *, gw_libc_poll_wait, int *);
int gw_host_guest_select(int, fd_set *, fd_set *, fd_set *, struct timespec *,
                         const sigset_t *, gw_libc_poll_wait, int *);
int gw_host_guest_epoll_wait(int, struct epoll_event *, int,
                             const struct timespec *, const sigset_t *,
                             gw_libc_poll_wait, int *);
int gw_host_guest_epoll_ctl(int, int, int, const struct epoll_event *, int *);
int gw_host_guest_descriptor(enum gw_libc_descriptor_op, int, int, int,
                             enum gw_libc_io_policy, int *);
#endif
