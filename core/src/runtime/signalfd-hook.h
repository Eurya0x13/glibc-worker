/* Optional logical descriptor callbacks for the owning GNU libc.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef GW_LIBC_SIGNALFD_H
#define GW_LIBC_SIGNALFD_H
#include "signal-consume-hook.h"
#include <sys/uio.h>

struct gw_libc_signalfd_hook {
  size_t size;
  int (*open)(int, const sigset_t *, int, int *handled);
  ssize_t (*read)(int, void *, size_t, gw_libc_wait_event, int *handled);
  ssize_t (*readv)(int, const struct iovec *, int, gw_libc_wait_event,
                   int *handled);
  uint64_t (*closing)(int);
  void (*closed)(uint64_t);
};

int __gw_domain_signalfd_hook(unsigned, const struct gw_libc_signalfd_hook *);
#endif
