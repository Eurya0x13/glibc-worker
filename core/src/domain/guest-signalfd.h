#ifndef GW_GUEST_SIGNALFD_H
#define GW_GUEST_SIGNALFD_H
#include "signal-targets.h"
#include "signalfd-hook.h"

/* Called only with the owning GNU libc and the current logical recipient. */
int gw_guest_signalfd(struct gw_signal_target *, int, const sigset_t *, int,
                      int *handled);
ssize_t gw_guest_signalfd_read(struct gw_signal_target *, int, void *, size_t,
                               enum gw_libc_io_policy, struct gw_libc_fd_wait,
                               int *handled);
ssize_t gw_guest_signalfd_readv(struct gw_signal_target *, int,
                                const struct iovec *, int,
                                enum gw_libc_io_policy, struct gw_libc_fd_wait,
                                int *handled);
int gw_guest_signalfd_stop(void);

/* Shared by descriptor bookkeeping and its readiness adapters. Hold the
 * writer only with catchable signals physically blocked. */
void gw_guest_fds_lock(void);
void gw_guest_fds_unlock(void);
uint64_t gw_guest_signalfd_cookie(int);
int gw_guest_signalfd_mask(uint64_t, uint64_t *);
int gw_guest_signalfd_native(uint64_t);
int gw_guest_signalfd_reap(void);
#endif
