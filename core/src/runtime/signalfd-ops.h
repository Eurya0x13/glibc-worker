/* Private libc call sites for the logical descriptor callbacks.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef GW_LIBC_SIGNALFD_OPS_H
#define GW_LIBC_SIGNALFD_OPS_H
#include <gw-signalfd-hook.h>

int __gw_domain_signalfd_open(int, const sigset_t *, int, int *);
ssize_t __gw_domain_signalfd_read(int, void *, size_t, int *);
ssize_t __gw_domain_signalfd_readv(int, const struct iovec *, int, int *);
uint64_t __gw_domain_signalfd_closing(int);
void __gw_domain_signalfd_closed(uint64_t);

libc_hidden_proto(__gw_domain_signalfd_open);
libc_hidden_proto(__gw_domain_signalfd_read);
libc_hidden_proto(__gw_domain_signalfd_readv);
libc_hidden_proto(__gw_domain_signalfd_closing);
libc_hidden_proto(__gw_domain_signalfd_closed);
#endif
