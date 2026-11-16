/* Signal-mask operation used by the owning glibc context assembly.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * https://github.com/bminor/glibc/blob/glibc-2.44/sysdeps/unix/sysv/linux/x86_64/setcontext.S
 * https://github.com/bminor/glibc/blob/glibc-2.44/sysdeps/unix/sysv/linux/aarch64/setcontext.S
 */
int __gw_context_sigmask(int, const sigset_t *, sigset_t *);

libc_hidden_proto(__gw_context_sigmask)

    int __gw_context_sigmask(int how, const sigset_t *set, sigset_t *oldset) {
  if (gw_logical_mask && *gw_mask_active) {
    return gw_domain_sigmask(how, set, oldset);
  }
  /* Unbound threads keep the original context-assembly syscall semantics,
   * including the kernel mask size and direct error reporting. */
  int result =
      INTERNAL_SYSCALL_CALL(rt_sigprocmask, how, set, oldset, __NSIG_BYTES);
  return INTERNAL_SYSCALL_ERROR_P(result) ? INTERNAL_SYSCALL_ERRNO(result) : 0;
}
libc_hidden_def(__gw_context_sigmask)
