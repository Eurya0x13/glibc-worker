/* Included by the owning GNU libc after the consuming wait callbacks.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <gw-signalfd-ops.h>
#include <futex-internal.h>
#include <sysdep-cancel.h>

static struct gw_libc_signalfd_hook gw_signalfd_hook;
static unsigned gw_signalfd_state;

int __gw_domain_signalfd_hook(unsigned version,
                              const struct gw_libc_signalfd_hook *hook) {
  if (version != GW_LIBC_SIGNALFD_VERSION || !hook ||
      hook->size != sizeof *hook || !hook->open || !hook->read ||
      !hook->readv || !hook->descriptor || !hook->poll || !hook->select ||
      !hook->epoll_ctl || !hook->epoll_wait || !hook->unwind ||
      !hook->restore || !hook->raw) {
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

static int signalfd_wait_cancelable(void *word, uint32_t sequence,
                                    const struct timespec *deadline) {
  return -__futex_abstimed_wait_cancelable64(word, sequence, CLOCK_MONOTONIC,
                                             deadline, FUTEX_PRIVATE);
}

static int signalfd_wait_nocancel(void *word, uint32_t sequence,
                                  const struct timespec *deadline) {
  return -__futex_abstimed_wait64(word, sequence, CLOCK_MONOTONIC, deadline,
                                  FUTEX_PRIVATE);
}

static int descriptor_wait(struct pollfd *fds, nfds_t count,
                           struct timespec *time, const void *mask) {
  return SYSCALL_CANCEL(ppoll, fds, count, time, mask, __NSIG_BYTES);
}

static int descriptor_wait_nocancel(struct pollfd *fds, nfds_t count,
                                    struct timespec *time, const void *mask) {
  return INLINE_SYSCALL_CALL(ppoll, fds, count, time, mask, __NSIG_BYTES);
}

static const struct gw_libc_fd_wait cancellable_io = {
    .futex = signalfd_wait_cancelable, .poll = descriptor_wait};
static const struct gw_libc_fd_wait noncancellable_io = {
    .futex = signalfd_wait_nocancel, .poll = descriptor_wait_nocancel};

int __gw_domain_signalfd_open(int fd, const sigset_t *set, int flags,
                              int *handled) {
  *handled = 0;
  return signalfd_hook_ready() ? gw_signalfd_hook.open(fd, set, flags, handled)
                               : 0;
}

libc_hidden_def(__gw_domain_signalfd_open);

ssize_t __gw_domain_signalfd_read(int fd, void *buf, size_t size,
                                  enum gw_libc_io_policy policy, int *handled) {
  *handled = 0;
  struct gw_libc_fd_wait wait =
      policy == GW_IO_CANCELLABLE ? cancellable_io : noncancellable_io;
  return signalfd_hook_ready()
             ? gw_signalfd_hook.read(fd, buf, size, policy, wait, handled)
             : 0;
}

libc_hidden_def(__gw_domain_signalfd_read);

ssize_t __gw_domain_signalfd_readv(int fd, const struct iovec *vectors,
                                   int count, int *handled) {
  *handled = 0;
  return signalfd_hook_ready()
             ? gw_signalfd_hook.readv(fd, vectors, count, GW_IO_CANCELLABLE,
                                      cancellable_io, handled)
             : 0;
}

libc_hidden_def(__gw_domain_signalfd_readv);

int __gw_domain_descriptor(enum gw_libc_descriptor_op op, int fd, int target,
                           int flags, enum gw_libc_io_policy policy,
                           int *handled) {
  *handled = 0;
  return signalfd_hook_ready() ? gw_signalfd_hook.descriptor(
                                     op, fd, target, flags, policy, handled)
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

extern __typeof__(syscall) __gw_domain_kernel_syscall;
long __gw_domain_descriptor_syscall(long, long, long, long, long, long,
                                    long) attribute_hidden;

long __gw_domain_descriptor_syscall(long number, long a, long b, long c, long d,
                                    long e, long f) {
  if (signalfd_hook_ready()) {
    static const struct gw_libc_raw_descriptor_io raw = {
        .mask_begin = __gw_domain_wait_begin,
        .mask_end = __gw_domain_wait_end,
        .wait = {.futex = signalfd_wait_nocancel,
                 .poll = descriptor_wait_nocancel}};
    const long arguments[] = {a, b, c, d, e, f};
    int handled = 0;
    long result = gw_signalfd_hook.raw(number, arguments, &raw, &handled);
    if (handled) {
      return result;
    }
  }
  return __gw_domain_kernel_syscall(number, a, b, c, d, e, f);
}
