"""Patch owning-libc send sites, retaining native descriptor validation."""

import shutil


def _install_send_extension(src, fragments, replace):
    shutil.copyfile(
        fragments / "signal-send.h", src / "include/gw-signal-send.h"
    )
    shutil.copyfile(
        fragments / "signal-send.inc", src / "nptl/gw-signal-send.inc"
    )
    shutil.copyfile(
        fragments / "signal-send-kill.c",
        src / "sysdeps/unix/sysv/linux/kill.c",
    )
    replace(
        src / "nptl/Versions",
        "__gw_domain_mask_bind;",
        "__gw_domain_mask_bind; __gw_domain_signal_send_hook;",
    )


def _patch_thread_kill(src, replace):
    kill = src / "nptl/pthread_kill.c"
    replace(
        kill,
        "#include <shlib-compat.h>",
        '#include <shlib-compat.h>\n#include "gw-signal-send.inc"',
    )
    replace(
        kill,
        "      int ret = INTERNAL_SYSCALL_CALL (tgkill, __getpid (), tid, signo);",
        "      int handled;\n"
        "      int status = __gw_domain_signal_send (pd, tid, signo, SI_TKILL,\n"
        "                                             NULL, &handled);\n"
        "      if (handled)\n"
        "        {\n"
        "          return status;\n"
        "        }\n"
        "      int ret = INTERNAL_SYSCALL_CALL (tgkill, __getpid (), tid, signo);",
    )
    replace(
        kill,
        "      ret = INTERNAL_SYSCALL_CALL (tgkill, __getpid (), pd->tid, signo);\n"
        "      ret = INTERNAL_SYSCALL_ERROR_P (ret) ? INTERNAL_SYSCALL_ERRNO (ret) : 0;",
        "      int handled;\n"
        "      ret = __gw_domain_signal_send (pd, pd->tid, signo, SI_TKILL,\n"
        "                                     NULL, &handled);\n"
        "      if (!handled)\n"
        "        {\n"
        "          ret = INTERNAL_SYSCALL_CALL (tgkill, __getpid (), pd->tid, signo);\n"
        "          ret = INTERNAL_SYSCALL_ERROR_P (ret) ? INTERNAL_SYSCALL_ERRNO (ret) : 0;\n"
        "        }",
    )


def _patch_thread_queue(src, replace):
    queue = src / "nptl/pthread_sigqueue.c"
    replace(
        queue,
        "#include <shlib-compat.h>",
        "#include <shlib-compat.h>\n#include <gw-signal-send.h>",
    )
    # Upstream pthread_sigqueue only snapshots tid. The extension must hold
    # exit_lock through publication/wake, just as pthread_kill does, so the
    # kernel cannot reuse a non-self target's TID between admission and wake.
    # https://sourceware.org/git/?p=glibc.git;a=blob;f=nptl/pthread_kill.c
    replace(
        queue,
        "  pid_t pid = getpid ();",
        "  if (__gw_domain_signal_send_enabled ())\n"
        "    {\n"
        "      internal_sigset_t old_mask;\n"
        "      int self = pd == THREAD_SELF;\n"
        "      if (!self)\n"
        "        {\n"
        "          internal_signal_block_all (&old_mask);\n"
        "          __libc_lock_lock (pd->exit_lock);\n"
        "        }\n"
        "      int handled = 0;\n"
        "      int status;\n"
        "      if (pd->exiting)\n"
        "        {\n"
        "          handled = 1;\n"
        "          status = ESRCH;\n"
        "        }\n"
        "      else\n"
        "        {\n"
        "          tid = self ? INTERNAL_SYSCALL_CALL (gettid) : pd->tid;\n"
        "          status = __gw_domain_signal_send (pd, tid, signo, SI_QUEUE,\n"
        "                                             &value, &handled);\n"
        "          if (!handled)\n"
        "            {\n"
        "              siginfo_t info = {0};\n"
        "              info.si_signo = signo;\n"
        "              info.si_code = SI_QUEUE;\n"
        "              info.si_pid = __getpid ();\n"
        "              info.si_uid = __getuid ();\n"
        "              info.si_value = value;\n"
        "              status = INTERNAL_SYSCALL_CALL (rt_tgsigqueueinfo,\n"
        "                  info.si_pid, tid, signo, &info);\n"
        "              status = INTERNAL_SYSCALL_ERROR_P (status)\n"
        "                ? INTERNAL_SYSCALL_ERRNO (status) : 0;\n"
        "            }\n"
        "        }\n"
        "      if (!self)\n"
        "        {\n"
        "          __libc_lock_unlock (pd->exit_lock);\n"
        "          internal_signal_restore_set (&old_mask);\n"
        "        }\n"
        "      return status;\n"
        "    }\n\n"
        "  pid_t pid = getpid ();",
    )


def _patch_process_queue(src, replace):
    queue = src / "sysdeps/unix/sysv/linux/sigqueue.c"
    replace(
        queue,
        "#include <sysdep.h>",
        "#include <sysdep.h>\n#include <gw-signal-send.h>",
    )
    replace(
        queue,
        "  siginfo_t info;",
        "  if (__gw_domain_signal_send_enabled () && pid == __getpid ())\n"
        "    {\n"
        "      int handled;\n"
        "      int status = __gw_domain_signal_send (NULL, 0, sig, SI_QUEUE,\n"
        "                                             &val, &handled);\n"
        "      if (handled)\n"
        "        {\n"
        "          if (status)\n"
        "            {\n"
        "              __set_errno (status);\n"
        "              return -1;\n"
        "            }\n"
        "          return 0;\n"
        "        }\n"
        "    }\n"
        "  siginfo_t info;",
    )


def patch_signal_sends(src, fragments, replace):
    _install_send_extension(src, fragments, replace)
    _patch_thread_kill(src, replace)
    _patch_thread_queue(src, replace)
    _patch_process_queue(src, replace)
