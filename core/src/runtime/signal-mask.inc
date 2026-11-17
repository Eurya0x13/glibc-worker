/* Private owning-libc thread mask adapter, included by pthread_sigmask.c.
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
typedef void (*gw_mask_observer)(uint64_t);
static gw_mask_observer gw_mask_hook;

int __gw_domain_mask_hook(unsigned version, gw_mask_observer observer) {
  if (version != 1 || !observer) {
    return EINVAL;
  }
  gw_mask_observer expected = NULL;
  return __atomic_compare_exchange_n(&gw_mask_hook, &expected, observer, 0,
                                     __ATOMIC_RELEASE, __ATOMIC_RELAXED)
             ? 0
             : EBUSY;
}

static void gw_mask_publish(uint64_t mask) {
  gw_mask_observer observer = __atomic_load_n(&gw_mask_hook, __ATOMIC_ACQUIRE);
  if (observer) {
    /* The owning runtime updates eligibility before the kernel can deliver
     * its self-notification. Its internal wake error is separate from the
     * already committed logical mask. Preserve pthread_sigmask's errno ABI. */
    int saved = errno;
    observer(mask);
    __set_errno(saved);
  }
}

int __gw_domain_mask_inherited(uint64_t *logical, uint64_t *physical) {
  struct pthread *self = THREAD_SELF;
  if (!logical || !physical) {
    return EINVAL;
  }
  if ((self->flags & ATTR_FLAG_GW_DOMAIN) || !self->gw_inherited_mask_valid) {
    return ENOTSUP;
  }
  *logical = self->gw_inherited_mask;
  *physical = self->gw_inherited_carrier;
  return 0;
}

/* NPTL copies the parent's mask (or an explicit attribute) before clone and
 * restores pd->sigmask only after the child lifecycle hook. Keep the logical
 * Guest snapshot separate from that native physical startup mask. A genuine
 * child owns its GNU thread and initially restores the Guest logical mask,
 * not its parent's Bionic carrier mask. A runtime binding in the child hook
 * can prepare a different physical mask for its own private notification.
 * https://github.com/bminor/glibc/blob/glibc-2.44/nptl/pthread_create.c
 * https://github.com/bminor/glibc/blob/glibc-2.44/nptl/descr.h
 */
void __gw_domain_mask_inherit(internal_sigset_t *native, uint64_t *logical,
                              int explicit_mask) {
  memcpy(logical, native, sizeof *logical);
  if (gw_logical_mask && *gw_mask_active) {
    if (!explicit_mask) {
      *logical = (*logical & ~gw_reserved_mask) |
                 (*gw_logical_mask & gw_reserved_mask);
    }
    memcpy(native, logical, sizeof *logical);
  }
}

int __gw_domain_mask_bind(uint64_t *logical, const uint64_t *carrier,
                          const int *active, uint64_t reserved) {
  const uint64_t forbidden =
      (UINT64_C(1) << (SIGKILL - 1)) | (UINT64_C(1) << (SIGSTOP - 1)) |
      (UINT64_C(1) << (SIGCANCEL - 1)) | (UINT64_C(1) << (SIGSETXID - 1));
  if (!logical || !carrier || !active || (reserved & forbidden)) {
    return EINVAL;
  }
  if (gw_logical_mask) {
    return EALREADY;
  }
  gw_logical_mask = logical;
  gw_carrier_mask = carrier;
  gw_mask_active = active;
  gw_reserved_mask = reserved;
  if (THREAD_SELF->gw_mask_starting && *active) {
    uint64_t kernel = (*logical & ~reserved) | (*carrier & reserved);
    memcpy(&THREAD_SELF->sigmask, &kernel, sizeof kernel);
  }
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
  if (set) {
    gw_mask_publish(next);
  }
  result = INTERNAL_SYSCALL_CALL(rt_sigprocmask, SIG_SETMASK, &kernel, NULL,
                                 __NSIG_BYTES);
  if (INTERNAL_SYSCALL_ERROR_P(result)) {
    *gw_logical_mask = previous;
    if (set) {
      gw_mask_publish(previous);
    }
    INTERNAL_SYSCALL_CALL(rt_sigprocmask, SIG_SETMASK, &physical, NULL,
                          __NSIG_BYTES);
    return INTERNAL_SYSCALL_ERRNO(result);
  }
  return 0;
}
