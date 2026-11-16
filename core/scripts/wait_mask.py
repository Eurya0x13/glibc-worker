"""Adapt the reviewed LP64 libc wait entrypoints without replacing syscalls."""

from pathlib import Path


def patch_wait_masks(source: Path):
    linux = source / "sysdeps/unix/sysv/linux"

    def replace(name, old, new):
        path = linux / (name + ".c")
        text = path.read_text()
        if text.count(old) != 1:
            raise SystemExit("Wait-mask patch mismatch: " + str(path))
        path.write_text(text.replace(old, new))

    for name in (
        "ppoll",
        "pselect",
        "epoll_pwait",
        "epoll_pwait2",
        "sigsuspend",
        "sigtimedwait",
        "sigpending",
        "signalfd",
    ):
        # A system-dependency include is present in all reviewed files.
        path = linux / (name + ".c")
        text = path.read_text()
        index = text.index("\n#include <sysdep")
        path.write_text(
            text[:index] + "\n#include <gw-wait-mask.h>" + text[index:]
        )

    # These are the LP64 time64 branches used by both supported architectures.
    # Leave all native time32 fallback code in place for reviewability.
    for name, old, new in (
        (
            "ppoll",
            "  return SYSCALL_CANCEL (ppoll_time64, fds, nfds, timeout, sigmask,\n"
            "\t\t\t __NSIG_BYTES);",
            "  GW_DOMAIN_WAIT_BEGIN (sigmask);\n"
            "  GW_DOMAIN_WAIT_RETURN (SYSCALL_CANCEL (ppoll_time64, fds, nfds,\n"
            "                                       timeout, sigmask, __NSIG_BYTES));",
        ),
        (
            "pselect",
            "  /* NB: This is required by ARGIFY used in x32 internal_syscallN.  */",
            "  GW_DOMAIN_WAIT_BEGIN (sigmask);\n"
            "  /* NB: This is required by ARGIFY used in x32 internal_syscallN.  */",
        ),
        (
            "pselect",
            "  return SYSCALL_CANCEL (pselect6_time64, nfds, readfds, writefds, exceptfds,\n"
            "\t\t\t timeout, data);",
            "  GW_DOMAIN_WAIT_RETURN (SYSCALL_CANCEL (pselect6_time64, nfds, readfds,\n"
            "                                       writefds, exceptfds, timeout, data));",
        ),
        (
            "epoll_pwait",
            "  return SYSCALL_CANCEL (epoll_pwait, epfd, events, maxevents,\n"
            "\t\t\t timeout, set, __NSIG_BYTES);",
            "  GW_DOMAIN_WAIT_BEGIN (set);\n"
            "  GW_DOMAIN_WAIT_RETURN (SYSCALL_CANCEL (epoll_pwait, epfd, events,\n"
            "                                       maxevents, timeout, set, __NSIG_BYTES));",
        ),
        (
            "epoll_pwait2",
            "  return SYSCALL_CANCEL (epoll_pwait2, fd, ev, maxev, tmo, s, __NSIG_BYTES);",
            "  GW_DOMAIN_WAIT_BEGIN (s);\n"
            "  GW_DOMAIN_WAIT_RETURN (SYSCALL_CANCEL (epoll_pwait2, fd, ev, maxev,\n"
            "                                       tmo, s, __NSIG_BYTES));",
        ),
        (
            "sigsuspend",
            "  return SYSCALL_CANCEL (rt_sigsuspend, set, __NSIG_BYTES);",
            "  GW_DOMAIN_WAIT_BEGIN (set);\n"
            "  GW_DOMAIN_WAIT_RETURN (SYSCALL_CANCEL (rt_sigsuspend, set, __NSIG_BYTES));",
        ),
        (
            "sigtimedwait",
            "  int result;",
            "  sigset_t filtered;\n"
            "  if (set && __gw_domain_wait_filter (set, &filtered))\n"
            "    set = &filtered;\n"
            "  int result;",
        ),
        (
            "signalfd",
            "  return INLINE_SYSCALL (signalfd4, 4, fd, mask, __NSIG_BYTES, flags);",
            "  sigset_t filtered;\n"
            "  if (mask && __gw_domain_wait_filter (mask, &filtered))\n"
            "    mask = &filtered;\n"
            "  return INLINE_SYSCALL (signalfd4, 4, fd, mask, __NSIG_BYTES, flags);",
        ),
        (
            "sigpending",
            "  return INLINE_SYSCALL_CALL (rt_sigpending, set, __NSIG_BYTES);",
            "  int result = INLINE_SYSCALL_CALL (rt_sigpending, set, __NSIG_BYTES);\n"
            "  if (result == 0)\n"
            "    __gw_domain_wait_filter (set, set);\n"
            "  return result;",
        ),
    ):
        replace(name, old, new)
