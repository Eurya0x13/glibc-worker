/* Private adopted-carrier mask adapter, included by pthread_sigmask.c.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Host-reserved signals remain governed by the suspended Host's kernel mask.
 * Their Guest mask is logical state: do not consume/requeue kernel signals or
 * manufacture siginfo. The kernel retains the sender and pending queue:
 * https://github.com/torvalds/linux/blob/v6.12/kernel/signal.c
 */
#include <stdint.h>
#include <string.h>

static __thread uint64_t *gw_logical_mask;
static __thread const uint64_t *gw_carrier_mask;
static __thread const int *gw_mask_active;
static __thread uint64_t gw_reserved_mask;

int __gw_domain_mask_bind(uint64_t *logical, const uint64_t *carrier,
                          const int *active, uint64_t reserved) {
  const uint64_t forbidden =
      (UINT64_C(1) << (SIGKILL - 1)) | (UINT64_C(1) << (SIGSTOP - 1)) |
      (UINT64_C(1) << (SIGCANCEL - 1)) | (UINT64_C(1) << (SIGSETXID - 1));
  if (!logical || !carrier || !active || (reserved & forbidden) ||
      !(THREAD_SELF->flags & ATTR_FLAG_GW_DOMAIN)) {
    return EINVAL;
  }
  if (gw_logical_mask) {
    return EALREADY;
  }
  gw_logical_mask = logical;
  gw_carrier_mask = carrier;
  gw_mask_active = active;
  gw_reserved_mask = reserved;
  return 0;
}

static int gw_domain_sigmask(int how, const sigset_t *set, sigset_t *oldset) {
  if (set && how != SIG_BLOCK && how != SIG_UNBLOCK && how != SIG_SETMASK) {
    return EINVAL;
  }
  uint64_t input = 0;
  if (set) {
    memcpy(&input, set, sizeof input);
  }
  uint64_t all = UINT64_MAX, physical;
  int result = INTERNAL_SYSCALL_CALL(rt_sigprocmask, SIG_SETMASK, &all,
                                     &physical, __NSIG_BYTES);
  if (INTERNAL_SYSCALL_ERROR_P(result)) {
    return INTERNAL_SYSCALL_ERRNO(result);
  }
  uint64_t previous =
      (physical & ~gw_reserved_mask) | (*gw_logical_mask & gw_reserved_mask);
  uint64_t next = previous;
  if (set) {
    next = how == SIG_BLOCK     ? previous | input
           : how == SIG_UNBLOCK ? previous & ~input
                                : input;
  }
  next &=
      ~((UINT64_C(1) << (SIGKILL - 1)) | (UINT64_C(1) << (SIGSTOP - 1)) |
        (UINT64_C(1) << (SIGCANCEL - 1)) | (UINT64_C(1) << (SIGSETXID - 1)));
  if (oldset) {
    memset(oldset, 0, sizeof *oldset);
    memcpy(oldset, &previous, sizeof previous);
  }
  uint64_t kernel =
      (next & ~gw_reserved_mask) | (*gw_carrier_mask & gw_reserved_mask);
  *gw_logical_mask = next;
  result = INTERNAL_SYSCALL_CALL(rt_sigprocmask, SIG_SETMASK, &kernel, NULL,
                                 __NSIG_BYTES);
  if (INTERNAL_SYSCALL_ERROR_P(result)) {
    *gw_logical_mask = previous;
    INTERNAL_SYSCALL_CALL(rt_sigprocmask, SIG_SETMASK, &physical, NULL,
                          __NSIG_BYTES);
    return INTERNAL_SYSCALL_ERRNO(result);
  }
  return 0;
}
