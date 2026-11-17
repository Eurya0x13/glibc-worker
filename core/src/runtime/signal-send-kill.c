/* Linux kill entry with optional same-process Guest routing.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sysdep.h>
#include <gw-signal-send.h>

int __kill(int pid, int number) {
  if (__gw_domain_signal_send_enabled() && pid == __getpid()) {
    int handled;
    int result =
        __gw_domain_signal_send(NULL, 0, number, SI_USER, NULL, &handled);
    if (handled) {
      if (result) {
        __set_errno(result);
        return -1;
      }
      return 0;
    }
  }

  /* External PIDs, process groups, signal zero and unhandled requests keep
   * the kernel's permission, existence and process-wide fatal semantics. */
  return INLINE_SYSCALL_CALL(kill, pid, number);
}

libc_hidden_def(__kill) weak_alias(__kill, kill)
