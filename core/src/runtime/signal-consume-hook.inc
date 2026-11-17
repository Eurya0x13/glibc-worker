/* Bootstrap-owned signal wait callbacks, included by pthread_sigmask.c.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <gw-wait-mask.h>

static struct gw_libc_signal_consume_hook gw_consume_hook;
static unsigned gw_consume_state;

int __gw_domain_signal_consume_hook(
    unsigned version, const struct gw_libc_signal_consume_hook *hook) {
  if (version != 1 || !hook || hook->size != sizeof *hook || !hook->pending ||
      !hook->wait) {
    return EINVAL;
  }
  unsigned expected = 0;
  if (!__atomic_compare_exchange_n(&gw_consume_state, &expected, 1, 0,
                                   __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
    return EBUSY;
  }
  gw_consume_hook = *hook;
  __atomic_store_n(&gw_consume_state, 2, __ATOMIC_RELEASE);
  return 0;
}

int __gw_domain_consume_pending(sigset_t *set, int *handled) {
  *handled = 0;
  return __atomic_load_n(&gw_consume_state, __ATOMIC_ACQUIRE) == 2
             ? gw_consume_hook.pending(set, handled)
             : 0;
}

libc_hidden_def(__gw_domain_consume_pending);

int __gw_domain_consume_wait(const sigset_t *set, siginfo_t *info,
                              const struct timespec *timeout,
                              gw_libc_wait_event wait_event, int *handled) {
  *handled = 0;
  return __atomic_load_n(&gw_consume_state, __ATOMIC_ACQUIRE) == 2
             ? gw_consume_hook.wait(set, info, timeout, wait_event, handled)
             : 0;
}

libc_hidden_def(__gw_domain_consume_wait);
