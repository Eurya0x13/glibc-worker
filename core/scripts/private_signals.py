"""Keep Guest NPTL's kernel-private signals outside the Host libc range."""


def patch_private_signals(src, replace):
    # Both libc instances share kernel dispositions. GNU's upstream 32/33
    # collide with Bionic's timer/backtrace channels, even before a module
    # installs its first public handler. The matching Host leases 63/64 for
    # this process-pinned runtime before it starts the GNU loader.
    # https://sourceware.org/git/?p=glibc.git;a=blob;f=sysdeps/unix/sysv/linux/internal-signals.h
    # https://android.googlesource.com/platform/bionic/+/android13-release/libc/platform/bionic/reserved_signals.h
    internal = src / "sysdeps/unix/sysv/linux/internal-signals.h"
    replace(
        internal,
        "#define SIGCANCEL       __SIGRTMIN",
        "#define SIGCANCEL       63",
    )
    replace(
        internal,
        "#define SIGSETXID       (__SIGRTMIN + 1)",
        "#define SIGSETXID       64",
    )
    replace(
        src / "signal/allocrtsig.c",
        "static int current_rtmax = __SIGRTMAX;",
        "static int current_rtmax = 62;",
    )

    # Keep public rejection of the other libc's private 32/33 channels. This
    # also preserves unmodified GNU callers' historical EINVAL behavior.
    replace(
        internal,
        "  return (sig == SIGCANCEL) || (sig == SIGSETXID);",
        "  return (sig == SIGCANCEL) || (sig == SIGSETXID)"
        " || sig == 32 || sig == 33;",
    )
    replace(
        internal,
        "  __sigdelset (set, SIGSETXID);",
        "  __sigdelset (set, SIGSETXID);\n"
        "  __sigdelset (set, 32);\n  __sigdelset (set, 33);",
    )
    replace(
        src / "nptl/pthread_sigqueue.c",
        "  if (signo == SIGCANCEL || signo == SIGTIMER || signo == SIGSETXID)",
        "  if (is_internal_signal (signo))",
    )
