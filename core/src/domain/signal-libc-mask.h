#ifndef GW_SIGNAL_LIBC_MASK_H
#define GW_SIGNAL_LIBC_MASK_H
#include <signal.h>
#include <stdint.h>

/* Match the owning libc's input filter before applying BLOCK/UNBLOCK/SETMASK.
 * Applying every operation as a filtered SETMASK would silently clear internal
 * bits already blocked by a native action mask. Queries must never do that.
 * Bionic reserves timer signal 32 as blocked and other private RTs as
 * unblocked; its runtime SIGRTMIN supplies the public boundary across Android
 * versions.
 * https://github.com/aosp-mirror/platform_bionic/blob/main/libc/platform/bionic/reserved_signals.h
 * https://github.com/bminor/glibc/blob/glibc-2.41/nptl/pthread_sigmask.c
 */
static inline uint64_t gw_signal_libc_mask(uint64_t input, int how) {
#ifdef __ANDROID__
  uint64_t timer = UINT64_C(1) << 31;
  if (how == SIG_UNBLOCK) {
    input &= ~timer;
  } else {
    input |= timer;
  }
  for (int number = 33; number < SIGRTMIN; ++number) {
    uint64_t bit = UINT64_C(1) << (number - 1);
    if (how == SIG_UNBLOCK) {
      input |= bit;
    } else {
      input &= ~bit;
    }
  }
#else
  (void)how;
  for (int number = 32; number < SIGRTMIN; ++number) {
    input &= ~(UINT64_C(1) << (number - 1));
  }
#endif
  return input;
}
#endif
