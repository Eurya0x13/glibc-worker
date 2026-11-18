/* Adopted Guest mask transactions for Linux's atomic mask-and-wait APIs.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Included beside signal-mask.inc so the binding stays private to libc.
 *
 * The kernel's temporary mask must retain the suspended Host's reserved bits.
 * Updating just the syscall argument would leave a Guest handler observing the
 * previous logical mask. Block delivery, publish the temporary logical mask,
 * then let the owning syscall atomically install its translated physical mask.
 * Its return restores the blocked transaction mask; finish restores both views.
 * A native siglongjmp/setcontext restores through the same logical mask
 * adapter. https://github.com/torvalds/linux/blob/v6.12/kernel/signal.c
 * https://github.com/torvalds/linux/blob/v6.12/fs/select.c
 */
#include <gw-wait-mask.h>

int __gw_domain_wait_begin(const sigset_t *set,
                           struct gw_domain_wait_mask *state) {
  state->active = 0;
  if (!set || !gw_logical_mask || !*gw_mask_active) {
    return 0;
  }
  uint64_t requested, all = UINT64_MAX;
  memcpy(&requested, set, sizeof requested);
  int result = INTERNAL_SYSCALL_CALL(rt_sigprocmask, SIG_SETMASK, &all,
                                     &state->physical, __NSIG_BYTES);
  if (INTERNAL_SYSCALL_ERROR_P(result)) {
    return INTERNAL_SYSCALL_ERRNO(result);
  }
  state->logical = (state->physical & ~gw_reserved_mask) |
                   (*gw_logical_mask & gw_reserved_mask);
  requested &=
      ~((UINT64_C(1) << (SIGKILL - 1)) | (UINT64_C(1) << (SIGSTOP - 1)) |
        (UINT64_C(1) << (SIGCANCEL - 1)) | (UINT64_C(1) << (SIGSETXID - 1)));
  *gw_logical_mask = requested;
  gw_mask_publish(requested);
  uint64_t kernel =
      (requested & ~gw_reserved_mask) | (*gw_carrier_mask & gw_reserved_mask);
  memset(&state->kernel, 0, sizeof state->kernel);
  memcpy(&state->kernel, &kernel, sizeof kernel);
  state->active = 1;
  return 0;
}

libc_hidden_def(__gw_domain_wait_begin);

int __gw_domain_wait_end(struct gw_domain_wait_mask *state) {
  if (!state->active) {
    return 0;
  }
  state->active = 0;
  uint64_t all = UINT64_MAX;
  int result = INTERNAL_SYSCALL_CALL(rt_sigprocmask, SIG_SETMASK, &all, NULL,
                                     __NSIG_BYTES);
  if (INTERNAL_SYSCALL_ERROR_P(result)) {
    return INTERNAL_SYSCALL_ERRNO(result);
  }
  *gw_logical_mask = state->logical;
  gw_mask_publish(state->logical);
  result = INTERNAL_SYSCALL_CALL(rt_sigprocmask, SIG_SETMASK, &state->physical,
                                 NULL, __NSIG_BYTES);
  return INTERNAL_SYSCALL_ERROR_P(result) ? INTERNAL_SYSCALL_ERRNO(result) : 0;
}

libc_hidden_def(__gw_domain_wait_end);

int __gw_domain_wait_filter(const sigset_t *set, sigset_t *filtered) {
  if (!gw_logical_mask || !*gw_mask_active) {
    return 0;
  }
  uint64_t mask;
  memcpy(&mask, set, sizeof mask);
  mask &= ~gw_reserved_mask;
  memset(filtered, 0, sizeof *filtered);
  memcpy(filtered, &mask, sizeof mask);
  return 1;
}

libc_hidden_def(__gw_domain_wait_filter);
