/* Optional consuming-wait entrypoints for the owning GNU libc.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef GW_LIBC_SIGNAL_CONSUME_H
#define GW_LIBC_SIGNAL_CONSUME_H
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* Zero or negative errno. The absolute deadline uses CLOCK_MONOTONIC.
 * The owning libc supplies cancellation at its actual futex syscall. */
typedef int (*gw_libc_wait_event)(void *, uint32_t, const struct timespec *);

struct gw_libc_signal_consume_hook {
  size_t size;
  /* Return ordinary libc results, including errno and application copyout.
   * A zero handled flag requests the ordinary native path instead. */
  int (*pending)(sigset_t *, int *handled);
  int (*wait)(const sigset_t *, siginfo_t *, const struct timespec *,
              gw_libc_wait_event, int *handled);
};

int __gw_domain_signal_consume_hook(unsigned,
                                    const struct gw_libc_signal_consume_hook *);
#endif
