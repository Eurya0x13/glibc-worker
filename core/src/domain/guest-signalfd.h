#ifndef GW_GUEST_SIGNALFD_H
#define GW_GUEST_SIGNALFD_H
#include "signal-targets.h"
#include "signalfd-hook.h"

/* Called only with the owning GNU libc and the current logical recipient. */
int gw_guest_signalfd(struct gw_signal_target *, int, const sigset_t *, int,
                      int *handled);
ssize_t gw_guest_signalfd_read(struct gw_signal_target *, int, void *, size_t,
                               gw_libc_wait_event, int *handled);
ssize_t gw_guest_signalfd_readv(struct gw_signal_target *, int,
                                const struct iovec *, int, gw_libc_wait_event,
                                int *handled);
uint64_t gw_guest_signalfd_closing(int);
void gw_guest_signalfd_closed(uint64_t);
int gw_guest_signalfd_stop(void);
#endif
