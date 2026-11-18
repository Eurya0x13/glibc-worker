/* Included by the owning GNU libc after the consuming wait callbacks.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <gw-signalfd-ops.h>
#include <futex-internal.h>
#include <sysdep-cancel.h>

static struct gw_libc_signalfd_hook gw_signalfd_hook;
static unsigned gw_signalfd_state;

int __gw_domain_signalfd_hook(unsigned version,
                              const struct gw_libc_signalfd_hook *hook) {
  if (version != 2 || !hook || hook->size != sizeof *hook || !hook->open ||
      !hook->read || !hook->readv || !hook->descriptor || !hook->poll ||
      !hook->select || !hook->epoll_ctl || !hook->epoll_wait || !hook->unwind ||
      !hook->restore) {
    return EINVAL;
  }
  unsigned expected = 0;
  if (!__atomic_compare_exchange_n(&gw_signalfd_state, &expected, 1, 0,
                                   __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
    return EBUSY;
  }

  gw_signalfd_hook = *hook;
  __atomic_store_n(&gw_signalfd_state, 2, __ATOMIC_RELEASE);
  return 0;
}

static int signalfd_hook_ready(void) {
  return __atomic_load_n(&gw_signalfd_state, __ATOMIC_ACQUIRE) == 2;
}

static int signalfd_wait(void *word, uint32_t sequence,
                         const struct timespec *deadline) {
  return -__futex_abstimed_wait_cancelable64(word, sequence, CLOCK_MONOTONIC,
                                             deadline, FUTEX_PRIVATE);
}

int __gw_domain_signalfd_open(int fd, const sigset_t *set, int flags,
                              int *handled) {
  *handled = 0;
  return signalfd_hook_ready() ? gw_signalfd_hook.open(fd, set, flags, handled)
                               : 0;
}

libc_hidden_def(__gw_domain_signalfd_open);

ssize_t __gw_domain_signalfd_read(int fd, void *buf, size_t size,
                                  int *handled) {
  *handled = 0;
  return signalfd_hook_ready()
             ? gw_signalfd_hook.read(fd, buf, size, signalfd_wait, handled)
             : 0;
}

libc_hidden_def(__gw_domain_signalfd_read);

ssize_t __gw_domain_signalfd_readv(int fd, const struct iovec *vectors,
                                   int count, int *handled) {
  *handled = 0;
  return signalfd_hook_ready() ? gw_signalfd_hook.readv(fd, vectors, count,
                                                        signalfd_wait, handled)
                               : 0;
}

libc_hidden_def(__gw_domain_signalfd_readv);

static int descriptor_wait(struct pollfd *fds, nfds_t count,
                           struct timespec *time, const void *mask) {
  return SYSCALL_CANCEL(ppoll, fds, count, time, mask, __NSIG_BYTES);
}

int __gw_domain_descriptor(enum gw_libc_descriptor_op op, int fd, int target,
                           int flags, int *handled) {
  *handled = 0;
  return signalfd_hook_ready()
             ? gw_signalfd_hook.descriptor(op, fd, target, flags, handled)
             : 0;
}

libc_hidden_def(__gw_domain_descriptor);

int __gw_domain_poll(struct pollfd *fds, nfds_t count, const struct timespec *t,
                     const sigset_t *mask, int *handled) {
  *handled = 0;
  return signalfd_hook_ready() ? gw_signalfd_hook.poll(fds, count, t, mask,
                                                       descriptor_wait, handled)
                               : 0;
}

libc_hidden_def(__gw_domain_poll);

int __gw_domain_select(int n, fd_set *r, fd_set *w, fd_set *x,
                       struct timespec *t, const sigset_t *mask, int *handled) {
  *handled = 0;
  return signalfd_hook_ready()
             ? gw_signalfd_hook.select(n, r, w, x, t, mask, descriptor_wait,
                                       handled)
             : 0;
}

libc_hidden_def(__gw_domain_select);

int __gw_domain_epoll_ctl(int epfd, int op, int fd,
                          const struct epoll_event *ev, int *handled) {
  *handled = 0;
  return signalfd_hook_ready()
             ? gw_signalfd_hook.epoll_ctl(epfd, op, fd, ev, handled)
             : 0;
}

libc_hidden_def(__gw_domain_epoll_ctl);

int __gw_domain_epoll_wait(int epfd, struct epoll_event *events, int n,
                           const struct timespec *t, const sigset_t *mask,
                           int *handled) {
  *handled = 0;
  return signalfd_hook_ready()
             ? gw_signalfd_hook.epoll_wait(epfd, events, n, t, mask,
                                           descriptor_wait, handled)
             : 0;
}

libc_hidden_def(__gw_domain_epoll_wait);

void __gw_domain_descriptor_unwind(uintptr_t sp, uintptr_t stack_top) {
  if (signalfd_hook_ready()) {
    gw_signalfd_hook.unwind(sp, stack_top);
  }
}

libc_hidden_def(__gw_domain_descriptor_unwind);

void __gw_domain_descriptor_restore(const ucontext_t *target) {
  if (!signalfd_hook_ready()) {
    return;
  }
  uintptr_t low = (uintptr_t)THREAD_SELF->stackblock;
  size_t size = THREAD_SELF->stackblock_size;
#ifdef __x86_64__
  uintptr_t sp = target->uc_mcontext.gregs[REG_RSP];
#else
  uintptr_t sp = target->uc_mcontext.sp;
#endif
  if (sp - low < size) {
    gw_signalfd_hook.restore(sp, low, size);
  }
}

libc_hidden_def(__gw_domain_descriptor_restore);
