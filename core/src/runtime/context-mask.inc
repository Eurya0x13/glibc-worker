/* Signal-mask operation used by the owning glibc context assembly.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * https://github.com/bminor/glibc/blob/master/sysdeps/unix/sysv/linux/x86_64/setcontext.S
 * https://github.com/bminor/glibc/blob/master/sysdeps/unix/sysv/linux/aarch64/setcontext.S
 */
#include <stddef.h>
#include <unistd.h>
#include <gw-context-events.h>

static gw_libc_context_hook gw_context_hook;
static __thread uint64_t gw_context_deferred_mask;
__thread const ucontext_t *__gw_context_deferred_target attribute_hidden
    attribute_tls_model_ie;

int __gw_domain_context_hook(unsigned, gw_libc_context_hook);
int __gw_context_sigmask(int, const sigset_t *, sigset_t *, const ucontext_t *);
void __gw_context_constructed(ucontext_t *);
void __gw_context_complete(void);

/* clang-format off */
libc_hidden_proto(__gw_context_sigmask)
libc_hidden_proto(__gw_context_constructed)
libc_hidden_proto(__gw_context_complete)
    /* clang-format on */

    int __gw_domain_context_hook(unsigned version, gw_libc_context_hook hook) {
  if (version != 3 || !hook || gw_context_hook) {
    return EINVAL;
  }
  gw_context_hook = hook;
  return 0;
}

void __gw_context_constructed(ucontext_t *context) {
  if (gw_context_hook) {
    gw_context_hook(GW_CONTEXT_CONSTRUCT, context, NULL, NULL);
  }
}

libc_hidden_def(__gw_context_constructed);

static int context_sigmask(int how, const sigset_t *set, sigset_t *oldset) {
  if (gw_logical_mask && *gw_mask_active) {
    return gw_domain_sigmask(how, set, oldset);
  }
  /* Unbound threads keep the original context-assembly syscall semantics,
   * including the kernel mask size and direct error reporting. */
  int result =
      INTERNAL_SYSCALL_CALL(rt_sigprocmask, how, set, oldset, __NSIG_BYTES);
  return INTERNAL_SYSCALL_ERROR_P(result) ? INTERNAL_SYSCALL_ERRNO(result) : 0;
}

static void context_physical_mask(const uint64_t *mask, uint64_t *previous) {
  int result = INTERNAL_SYSCALL_CALL(rt_sigprocmask, SIG_SETMASK, mask,
                                     previous, __NSIG_BYTES);
  if (INTERNAL_SYSCALL_ERROR_P(result)) {
    _exit(125);
  }
}

static void defer_context_mask(ucontext_t *saved, const ucontext_t *target,
                               uint64_t previous) {
  if (!gw_logical_mask || !*gw_mask_active) {
    _exit(125);
  }
  if (saved) {
    uint64_t logical =
        (previous & ~gw_reserved_mask) | (*gw_logical_mask & gw_reserved_mask);
    memset(&saved->uc_sigmask, 0, sizeof saved->uc_sigmask);
    memcpy(&saved->uc_sigmask, &logical, sizeof logical);
    gw_context_hook(GW_CONTEXT_CAPTURE, saved, NULL, &previous);
  }

  gw_context_hook(GW_CONTEXT_COMMIT, saved, target, &previous);
  uint64_t next;
  memcpy(&next, &target->uc_sigmask, sizeof next);
  next &=
      ~((UINT64_C(1) << (SIGKILL - 1)) | (UINT64_C(1) << (SIGSTOP - 1)) |
        (UINT64_C(1) << (SIGCANCEL - 1)) | (UINT64_C(1) << (SIGSETXID - 1)));
  *gw_logical_mask = next;
  gw_mask_publish(next);
  gw_context_deferred_mask =
      (next & ~gw_reserved_mask) | (*gw_carrier_mask & gw_reserved_mask);
  __gw_context_deferred_target = target;
}

/* Runs on the restored native stack. Clear the pending slot before the
 * syscall can admit a nested signal or another context switch. */
void __gw_context_complete(void) {
  uint64_t mask = gw_context_deferred_mask;
  __gw_context_deferred_target = NULL;
  context_physical_mask(&mask, NULL);
}

libc_hidden_def(__gw_context_complete);

int __gw_context_sigmask(int how, const sigset_t *set, sigset_t *oldset,
                         const ucontext_t *swap_target) {
  /* The assembly has already saved its caller's synchronous machine state.
   * ARM64 swapcontext saves the old mask here and tail-calls setcontext. */
  const ucontext_t *target = swap_target;
  if (set) {
    target =
        (const void *)((const char *)set - offsetof(ucontext_t, uc_sigmask));
  }
  ucontext_t *saved = NULL;
  if (oldset) {
    saved = (void *)((char *)oldset - offsetof(ucontext_t, uc_sigmask));
  }
  if (set && !saved && __gw_context_deferred_target == target) {
    return 0; /* ARM64 swapcontext tail-enters the native restore. */
  }
  if (gw_context_hook && target) {
    enum gw_context_operation operation =
        saved ? GW_CONTEXT_SWAP : GW_CONTEXT_RESTORE;
    uint64_t previous, all = UINT64_MAX;
    context_physical_mask(&all, &previous);
    int error = gw_context_hook(operation, saved, target, &previous);
    if (error == GW_CONTEXT_DEFER_MASK) {
      defer_context_mask(saved, target, previous);
      return 0;
    }
    context_physical_mask(&previous, NULL);
    if (error) {
      return error;
    }
  }

  int error = context_sigmask(how, set, oldset);
  if (!error && gw_context_hook && saved) {
    error = gw_context_hook(GW_CONTEXT_CAPTURE, saved, NULL, NULL);
  }
  return error;
}

libc_hidden_def(__gw_context_sigmask);
