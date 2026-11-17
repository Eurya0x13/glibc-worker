/* Signal-mask operation used by the owning glibc context assembly.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * https://github.com/bminor/glibc/blob/master/sysdeps/unix/sysv/linux/x86_64/setcontext.S
 * https://github.com/bminor/glibc/blob/master/sysdeps/unix/sysv/linux/aarch64/setcontext.S
 */
#include <stddef.h>
#include <gw-context-events.h>

static gw_libc_context_hook gw_context_hook;

int __gw_domain_context_hook(unsigned, gw_libc_context_hook);
int __gw_context_sigmask(int, const sigset_t *, sigset_t *, const ucontext_t *);
void __gw_context_constructed(ucontext_t *);

libc_hidden_proto(__gw_context_sigmask)
    libc_hidden_proto(__gw_context_constructed)

        int __gw_domain_context_hook(unsigned version,
                                     gw_libc_context_hook hook) {
  if (version != 2 || !hook || gw_context_hook) {
    return EINVAL;
  }
  gw_context_hook = hook;
  return 0;
}

void __gw_context_constructed(ucontext_t *context) {
  if (gw_context_hook) {
    gw_context_hook(GW_CONTEXT_CONSTRUCT, context, NULL);
  }
}

libc_hidden_def(__gw_context_constructed)

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
  if (gw_context_hook && target) {
    enum gw_context_operation operation =
        saved ? GW_CONTEXT_SWAP : GW_CONTEXT_RESTORE;
    int error = gw_context_hook(operation, saved, target);
    if (error) {
      return error;
    }
  }

  int error = context_sigmask(how, set, oldset);
  if (!error && gw_context_hook && saved) {
    error = gw_context_hook(GW_CONTEXT_CAPTURE, saved, NULL);
  }
  return error;
}
libc_hidden_def(__gw_context_sigmask)
