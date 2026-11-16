/* Experimental Guest libc policy. Host-owned dispositions are process-global;
   neither registration nor queries may expose a Host function pointer as a
   callable Guest handler. Ownership can only grow during runtime lifetime. */
#include <stdint.h>

static uint64_t gw_host_signals;
static int (*gw_guest_sigaction)(int, const void *, void *, int *);

int __gw_domain_sigaction_hook(int (*hook)(int, const void *, void *, int *)) {
  if (!hook || gw_guest_sigaction) {
    return EINVAL;
  }
  gw_guest_sigaction = hook;
  return 0;
}

int __gw_domain_reserve_signals(uint64_t mask) {
  const uint64_t forbidden = (UINT64_C(1) << (SIGKILL - 1)) |
                             (UINT64_C(1) << (SIGSTOP - 1)) |
                             (UINT64_C(1) << 31) | (UINT64_C(1) << 32);
  if (mask & forbidden) {
    return EINVAL;
  }
  __atomic_fetch_or(&gw_host_signals, mask, __ATOMIC_RELEASE);
  return 0;
}

static int gw_signal_is_host_owned(int sig) {
  return sig > 0 && sig <= 64 &&
         (__atomic_load_n(&gw_host_signals, __ATOMIC_ACQUIRE) &
          (UINT64_C(1) << (sig - 1)));
}
