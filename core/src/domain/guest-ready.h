#ifndef GW_GUEST_READY_H
#define GW_GUEST_READY_H
#include "signal-targets.h"
#include "signalfd-hook.h"

int gw_guest_descriptor(struct gw_signal_targets *, enum gw_libc_descriptor_op,
                        int, int, int, enum gw_libc_io_policy, int *);
int gw_guest_poll(struct gw_signal_target *, struct pollfd *, nfds_t,
                  const struct timespec *, const sigset_t *, gw_libc_poll_wait,
                  enum gw_libc_io_policy, struct timespec *, int *);
int gw_guest_select(struct gw_signal_target *, int, fd_set *, fd_set *,
                    fd_set *, struct timespec *, const sigset_t *,
                    gw_libc_poll_wait, enum gw_libc_io_policy, int *);
int gw_guest_epoll_wait(struct gw_signal_target *, int, struct epoll_event *,
                        int, const struct timespec *, const sigset_t *,
                        gw_libc_poll_wait, enum gw_libc_io_policy);
#endif
