/* Private libc call sites for the logical descriptor callbacks.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef GW_LIBC_SIGNALFD_OPS_H
#define GW_LIBC_SIGNALFD_OPS_H
#include <gw-signalfd-hook.h>
#include <ucontext.h>

int __gw_domain_signalfd_open(int, const sigset_t *, int, int *);
ssize_t __gw_domain_signalfd_read(int, void *, size_t, int *);
ssize_t __gw_domain_signalfd_readv(int, const struct iovec *, int, int *);
int __gw_domain_descriptor(enum gw_libc_descriptor_op, int, int, int, int *);
int __gw_domain_poll(struct pollfd *, nfds_t, const struct timespec *,
                     const sigset_t *, int *);
int __gw_domain_select(int, fd_set *, fd_set *, fd_set *, struct timespec *,
                       const sigset_t *, int *);
int __gw_domain_epoll_ctl(int, int, int, const struct epoll_event *, int *);
int __gw_domain_epoll_wait(int, struct epoll_event *, int,
                           const struct timespec *, const sigset_t *, int *);
void __gw_domain_descriptor_unwind(uintptr_t, uintptr_t);
void __gw_domain_descriptor_restore(const ucontext_t *);

libc_hidden_proto(__gw_domain_signalfd_open);
libc_hidden_proto(__gw_domain_signalfd_read);
libc_hidden_proto(__gw_domain_signalfd_readv);
libc_hidden_proto(__gw_domain_descriptor);
libc_hidden_proto(__gw_domain_poll);
libc_hidden_proto(__gw_domain_select);
libc_hidden_proto(__gw_domain_epoll_ctl);
libc_hidden_proto(__gw_domain_epoll_wait);
libc_hidden_proto(__gw_domain_descriptor_unwind);
libc_hidden_proto(__gw_domain_descriptor_restore);
#endif
