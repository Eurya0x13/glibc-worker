"""Connect the owning GNU libc's descriptor and readiness entrypoints."""

from pathlib import Path


def prepend_call(path, replace, anchor, call):
    replace(path, anchor, call + anchor)


def write_syscall_wrapper(
    path, name, parameters, setup, arguments, *, hidden=False
):
    path.write_text(
        "/* SPDX-License-Identifier: LGPL-2.1-or-later */\n"
        "#include <gw-signalfd-ops.h>\n"
        "#include <unistd.h>\n"
        "#include <sysdep.h>\n\n"
        f"int\n{('__' if hidden else '') + name} ({parameters})\n{{\n"
        f"{setup}"
        f"  return INLINE_SYSCALL_CALL ({name}, {arguments});\n"
        "}\n"
        + (
            f"libc_hidden_def (__{name})\nweak_alias (__{name}, {name})\n"
            if hidden
            else ""
        )
    )


def handled_call(call, *, returns="return result;", name="result", typ="int"):
    return (
        "  int handled;\n"
        f"  {typ} {name} = {call};\n"
        "  if (handled)\n"
        f"    {returns}\n\n"
    )


def patch_descriptors(linux, replace):
    for name in ("signalfd", "read", "readv", "close", "dup2"):
        path = linux / (name + ".c")
        text = path.read_text()
        index = text.index("\n#include <sysdep")
        path.write_text(
            text[:index] + "\n#include <gw-signalfd-ops.h>" + text[index:]
        )

    for name, anchor, call, result_type in (
        (
            "signalfd",
            "  sigset_t filtered;",
            "__gw_domain_signalfd_open (fd, mask, flags, &handled)",
            "int",
        ),
        (
            "read",
            "  return SYSCALL_CANCEL (read, fd, buf, nbytes);",
            "__gw_domain_signalfd_read (fd, buf, nbytes, GW_IO_CANCELLABLE, &handled)",
            "ssize_t",
        ),
        (
            "readv",
            "  return SYSCALL_CANCEL (readv, fd, iov, iovcnt);",
            "__gw_domain_signalfd_readv (fd, iov, iovcnt, &handled)",
            "ssize_t",
        ),
        (
            "close",
            "  return SYSCALL_CANCEL (close, fd);",
            "__gw_domain_descriptor (GW_DESCRIPTOR_CLOSE, fd, 0, 0, "
            "GW_IO_CANCELLABLE, &handled)",
            "int",
        ),
        (
            "dup2",
            "#ifdef __NR_dup2",
            "__gw_domain_descriptor (GW_DESCRIPTOR_DUP2, fd, fd2, 0, "
            "GW_IO_NOCANCEL, &handled)",
            "int",
        ),
    ):
        prepend_call(
            linux / (name + ".c"),
            replace,
            anchor,
            handled_call(call, typ=result_type),
        )

    write_syscall_wrapper(
        linux / "dup3.c",
        "dup3",
        "int fd, int fd2, int flags",
        handled_call(
            "__gw_domain_descriptor (GW_DESCRIPTOR_DUP3, fd, fd2, flags, "
            "GW_IO_NOCANCEL, &handled)"
        ),
        "fd, fd2, flags",
        hidden=True,
    )
    write_syscall_wrapper(
        linux / "epoll_ctl.c",
        "epoll_ctl",
        "int epfd, int op, int fd, struct epoll_event *event",
        handled_call("__gw_domain_epoll_ctl (epfd, op, fd, event, &handled)"),
        "epfd, op, fd, event",
    )
    # EXTRA syscall entries bypass C sources unless the routine is listed
    # in a subdirectory, as epoll_wait already is.
    replace(
        linux / "syscalls.list",
        "epoll_ctl\tEXTRA\tepoll_ctl\ti:iiip\tepoll_ctl\n",
        "",
    )
    entry = "  epoll_create " + "\\" + "\n"
    replace(linux / "Makefile", entry, entry + "  epoll_ctl " + "\\" + "\n")

    misc = "ifeq ($(subdir),misc)\n"
    replace(
        linux / "Makefile",
        misc,
        misc + "CFLAGS-epoll_pwait.c += -fexceptions\n"
        "CFLAGS-epoll_pwait2.c += -fexceptions\n",
    )


def patch_private_io(linux, replace):
    # The discovery link would otherwise pull the libc variants into rtld
    # before recompiling its libc dependencies as rtld objects.
    line_end = " " + "\\" + "\n"
    close_routines = (
        "close_nocancel",
        "close_nocancel_nostatus",
        "read_nocancel",
    )
    entry = "sysdep-rtld-routines +=" + line_end
    replace(
        linux / "Makefile",
        entry,
        entry + "".join("  " + name + line_end for name in close_routines),
    )

    close = "__gw_domain_descriptor (GW_DESCRIPTOR_CLOSE, fd, 0, 0, "
    for name, anchor, dispatch in (
        (
            "read_nocancel",
            "  return INLINE_SYSCALL_CALL (read, fd, buf, nbytes);",
            handled_call(
                "__gw_domain_signalfd_read (fd, buf, nbytes, "
                "GW_IO_NOCANCEL, &handled)",
                typ="ssize_t",
            ),
        ),
        (
            "close_nocancel",
            "  return INLINE_SYSCALL_CALL (close, fd);",
            handled_call(close + "GW_IO_NOCANCEL, &handled)"),
        ),
        (
            "close_nocancel_nostatus",
            "  INTERNAL_SYSCALL_CALL (close, fd);",
            "  int handled, saved = errno;\n"
            f"  {close}GW_IO_NOCANCEL, &handled);\n"
            "  __set_errno (saved);\n"
            "  if (handled)\n"
            "    return;\n",
        ),
    ):
        path = linux / (name + ".c")
        # rtld also compiles these sources and uses them before libc and TLS
        # are initialized. Its calls still go directly to the kernel.
        include = "#include <not-cancel.h>\n"
        replace(
            path,
            include,
            include
            + "#if IS_IN (libc)\n#include <gw-signalfd-ops.h>\n#endif\n",
        )
        prepend_call(
            path, replace, anchor, "#if IS_IN (libc)\n" + dispatch + "#endif\n"
        )


def milliseconds(timeout):
    return (
        "  const struct timespec *gw_limit = NULL;\n"
        "  struct timespec gw_interval;\n"
        f"  if ({timeout} >= 0)\n"
        "    {\n"
        f"      gw_interval.tv_sec = {timeout} / 1000;\n"
        f"      gw_interval.tv_nsec = ({timeout} % 1000) * 1000000;\n"
        "      gw_limit = &gw_interval;\n"
        "    }\n"
    )


def patch_readiness(linux, replace):
    for name in (
        "poll",
        "ppoll",
        "select",
        "pselect",
        "epoll_wait",
        "epoll_pwait",
        "epoll_pwait2",
    ):
        path = linux / (name + ".c")
        data = path.read_text()
        end = data.index("\n#include ")
        path.write_text(
            data[:end] + "\n#include <gw-signalfd-ops.h>" + data[end:]
        )

    prepend_call(
        linux / "poll.c",
        replace,
        "#ifdef __NR_poll",
        milliseconds("timeout")
        + handled_call(
            "__gw_domain_poll (fds, nfds, gw_limit, NULL, &handled)"
        ),
    )
    prepend_call(
        linux / "ppoll.c",
        replace,
        "  GW_DOMAIN_WAIT_RETURN (SYSCALL_CANCEL (ppoll_time64",
        handled_call(
            "__gw_domain_poll (fds, nfds, timeout, sigmask, &handled)",
            returns="GW_DOMAIN_WAIT_RETURN (gw_status);",
            name="gw_status",
        ),
    )
    prepend_call(
        linux / "select.c",
        replace,
        "  int r = SYSCALL_CANCEL (pselect6_time64, nfds, readfds, writefds, exceptfds,\n"
        "\t\t\t  pts64, NULL);",
        "  int handled;\n"
        "  int selected = __gw_domain_select (nfds, readfds, writefds, exceptfds,\n"
        "                                     pts64, NULL, &handled);\n"
        "  if (handled)\n"
        "    {\n"
        "      if (timeout != NULL)\n"
        "        TIMESPEC_TO_TIMEVAL (timeout, pts64);\n"
        "      return selected;\n"
        "    }\n\n",
    )
    prepend_call(
        linux / "pselect.c",
        replace,
        "  /* NB: This is required by ARGIFY used in x32 internal_syscallN.  */",
        "  struct timespec gw_interval;\n"
        "  if (timeout)\n"
        "    gw_interval = *timeout;\n"
        + handled_call(
            "__gw_domain_select (nfds, readfds, writefds, exceptfds,\n"
            "                                      timeout ? &gw_interval : NULL,\n"
            "                                      sigmask, &handled)",
            returns="GW_DOMAIN_WAIT_RETURN (gw_status);",
            name="gw_status",
        ),
    )
    prepend_call(
        linux / "epoll_wait.c",
        replace,
        "#ifdef __NR_epoll_wait",
        milliseconds("timeout")
        + handled_call(
            "__gw_domain_epoll_wait (epfd, events, maxevents, gw_limit, NULL, &handled)"
        ),
    )
    prepend_call(
        linux / "epoll_pwait.c",
        replace,
        "  GW_DOMAIN_WAIT_RETURN (SYSCALL_CANCEL (epoll_pwait,",
        milliseconds("timeout")
        + handled_call(
            "__gw_domain_epoll_wait (epfd, events, maxevents, gw_limit, set, &handled)",
            returns="GW_DOMAIN_WAIT_RETURN (gw_status);",
            name="gw_status",
        ),
    )
    prepend_call(
        linux / "epoll_pwait2.c",
        replace,
        "  GW_DOMAIN_WAIT_RETURN (SYSCALL_CANCEL (epoll_pwait2,",
        handled_call(
            "__gw_domain_epoll_wait (fd, ev, maxev, tmo, s, &handled)",
            returns="GW_DOMAIN_WAIT_RETURN (gw_status);",
            name="gw_status",
        ),
    )


def patch_signalfd(source: Path, replace):
    linux = source / "sysdeps/unix/sysv/linux"
    patch_descriptors(linux, replace)
    patch_private_io(linux, replace)
    patch_readiness(linux, replace)
