#ifndef GW_NONLOCAL_MASK_H
#define GW_NONLOCAL_MASK_H
#include <signal.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <unistd.h>

/* Internal Linux critical windows, not logical Host/Guest mask virtualization.
 * Use kernel sigset size (64 signals), not either libc's sigset_t size.
 * Called only with the executing domain's valid TLS, outside TP transitions.
 * https://github.com/torvalds/linux/blob/v6.12/kernel/signal.c
 */
static inline uint64_t gw_nonlocal_mask_block(void) {
  uint64_t previous, all = UINT64_MAX;
  if (syscall(SYS_rt_sigprocmask, SIG_SETMASK, &all, &previous, 8)) {
    _exit(125);
  }
  return previous;
}

static inline void gw_nonlocal_mask_restore(uint64_t mask) {
  if (syscall(SYS_rt_sigprocmask, SIG_SETMASK, &mask, NULL, 8)) {
    _exit(125);
  }
}
#endif
