/* Included by the owning GNU libc after the consuming wait callbacks.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <gw-signalfd-ops.h>
#include <futex-internal.h>

static struct gw_libc_signalfd_hook gw_signalfd_hook;
static unsigned gw_signalfd_state;

int __gw_domain_signalfd_hook(unsigned version,
                              const struct gw_libc_signalfd_hook *hook) {
  if (version != 1 || !hook || hook->size != sizeof *hook || !hook->open ||
      !hook->read || !hook->readv || !hook->closing || !hook->closed) {
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

uint64_t __gw_domain_signalfd_closing(int fd) {
  return signalfd_hook_ready() ? gw_signalfd_hook.closing(fd) : 0;
}

libc_hidden_def(__gw_domain_signalfd_closing);

void __gw_domain_signalfd_closed(uint64_t cookie) {
  if (cookie) {
    gw_signalfd_hook.closed(cookie);
  }
}

libc_hidden_def(__gw_domain_signalfd_closed);
