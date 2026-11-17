#ifndef GW_SIGNAL_CONSUMING_H
#define GW_SIGNAL_CONSUMING_H
#include "signal-targets.h"
#include "signal-consume-hook.h"

/* Owning GNU libc support for blocked Guest signal sets. These functions
 * implement the public pointer and errno contract. The integration owns a
 * live recipient for the entire call and prepares the kernel copy backend. */
int gw_guest_sigpending(struct gw_signal_target *, sigset_t *);
int gw_guest_sigtimedwait(struct gw_signal_target *, const sigset_t *,
                          siginfo_t *, const struct timespec *,
                          gw_libc_wait_event);
#endif
