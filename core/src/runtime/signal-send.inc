/* Optional Guest send routing, included by the owning pthread_kill.c.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <errno.h>
#include <string.h>
#include <gw-signal-send.h>

static gw_libc_signal_sender gw_signal_sender;

int __gw_domain_signal_send_hook(unsigned version,
                                 gw_libc_signal_sender sender) {
  if (version != 1 || !sender) {
    return EINVAL;
  }

  gw_libc_signal_sender expected = NULL;
  if (!__atomic_compare_exchange_n(&gw_signal_sender, &expected, sender, 0,
                                   __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
    return EBUSY;
  }
  return 0;
}

int __gw_domain_signal_send_enabled(void) {
  return __atomic_load_n(&gw_signal_sender, __ATOMIC_ACQUIRE) != NULL;
}

int __gw_domain_signal_send(void *thread, int tid, int number, int code,
                            const union sigval *value, int *handled) {
  *handled = 0;
  gw_libc_signal_sender sender =
      __atomic_load_n(&gw_signal_sender, __ATOMIC_ACQUIRE);
  if (!sender || number < 0 || number >= NSIG || number == SIGKILL ||
      number == SIGSTOP || is_internal_signal(number) || (!thread && !number)) {
    return 0;
  }

  /* pthread interfaces return an error number without changing errno. This
   * also protects success and native fallthrough from callback internals. */
  int saved = errno;
  internal_sigset_t previous;
  internal_signal_block_all(&previous);
  siginfo_t information;
  memset(&information, 0, sizeof information);
  information.si_signo = number;
  information.si_code = code;
  information.si_pid = __getpid();
  information.si_uid = __getuid();
  if (value) {
    information.si_value = *value;
  }

  int result = sender(thread, tid, &information, handled);
  /* Restore internal errno before unmasking. A real self-handler may then
   * change errno just as it can during native pthread_kill/raise; restoring
   * it after unmasking would silently overwrite the handler's own effect. */
  __set_errno(saved);
  internal_signal_restore_set(&previous);
  return result;
}
