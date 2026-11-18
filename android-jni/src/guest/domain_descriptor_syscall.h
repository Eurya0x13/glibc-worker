#ifndef GW_DOMAIN_DESCRIPTOR_SYSCALL_H
#define GW_DOMAIN_DESCRIPTOR_SYSCALL_H
#include "signalfd-hook.h"

struct gw_signal_target;

long gw_guest_descriptor_syscall(struct gw_signal_target *, long, const long[6],
                                 const struct gw_libc_raw_descriptor_io *,
                                 int *);
#endif
