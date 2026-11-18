"""Connect owning-libc descriptor entrypoints to optional Guest consumers."""

from pathlib import Path


def patch_signalfd(source: Path, replace):
    linux = source / "sysdeps/unix/sysv/linux"
    for name in ("signalfd", "read", "readv", "close"):
        path = linux / (name + ".c")
        text = path.read_text()
        index = text.index("\n#include <sysdep")
        path.write_text(
            text[:index] + "\n#include <gw-signalfd-ops.h>" + text[index:]
        )

    replace(
        linux / "signalfd.c",
        "  sigset_t filtered;",
        "  int handled;\n"
        "  int result = __gw_domain_signalfd_open (fd, mask, flags, &handled);\n"
        "  if (handled)\n"
        "    return result;\n\n"
        "  sigset_t filtered;",
    )
    replace(
        linux / "read.c",
        "  return SYSCALL_CANCEL (read, fd, buf, nbytes);",
        "  int handled;\n"
        "  ssize_t result = __gw_domain_signalfd_read (fd, buf, nbytes, &handled);\n"
        "  if (handled)\n"
        "    return result;\n"
        "  return SYSCALL_CANCEL (read, fd, buf, nbytes);",
    )
    replace(
        linux / "readv.c",
        "  return SYSCALL_CANCEL (readv, fd, iov, iovcnt);",
        "  int handled;\n"
        "  ssize_t result = __gw_domain_signalfd_readv (fd, iov, iovcnt,"
        " &handled);\n"
        "  if (handled)\n"
        "    return result;\n"
        "  return SYSCALL_CANCEL (readv, fd, iov, iovcnt);",
    )
    replace(
        linux / "close.c",
        "  return SYSCALL_CANCEL (close, fd);",
        "  uint64_t cookie = __gw_domain_signalfd_closing (fd);\n"
        "  int result = SYSCALL_CANCEL (close, fd);\n"
        "  __gw_domain_signalfd_closed (cookie);\n"
        "  return result;",
    )
