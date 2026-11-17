/* Optional Guest send extension, compiled only into the matched glibc.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef GW_LIBC_SIGNAL_SEND_H
#define GW_LIBC_SIGNAL_SEND_H
#include <signal.h>

/* A thread target is an owning-libc pthread descriptor protected by its
 * exit_lock (except the current thread, whose lifetime is already held).
 * NULL denotes this process's shared Guest queue. No pointer may escape the
 * call without the integration's separate recipient lifetime admission.
 * The callback returns a positive error, not -1/errno; handled=0 requests the
 * unmodified native send. Number zero probes a thread and never publishes.
 * The callback has all signals physically blocked. Restore internal errno
 * before unmasking so a self-handler's own errno effect remains visible.
 * Registration is bootstrap-only, immutable, and optional for old services. */
typedef int (*gw_libc_signal_sender)(void *, int, const siginfo_t *, int *);
int __gw_domain_signal_send_hook(unsigned, gw_libc_signal_sender);
int __gw_domain_signal_send(void *, int, int, int, const union sigval *,
                            int *) attribute_hidden;
int __gw_domain_signal_send_enabled(void) attribute_hidden;
#endif
