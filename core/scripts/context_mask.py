"""Keep glibc's native context assembly and route only its mask operation."""

from pathlib import Path


def patch_context_masks(source: Path):
    """Patch the reviewed x86_64/ARM64 get/set/swapcontext mask syscalls.

    Register, stack, floating-point, CET and GCS handling remains in the owning
    glibc assembly. A C wrapper around getcontext would capture a dead frame.
    The internal helper returns a pthread-style errno; negate it to retain the
    assembly's existing kernel-error path.
    """
    linux = source / "sysdeps/unix/sysv/linux"
    for arch in ("x86_64", "aarch64"):
        for name in ("getcontext", "setcontext", "swapcontext"):
            path = linux / arch / (name + ".S")
            text = path.read_text()
            if arch == "x86_64":
                old = (
                    "\tmovl\t$_NSIG8,%r10d\n"
                    "\tmovl\t$__NR_rt_sigprocmask, %eax\n\tsyscall"
                )
                if name == "setcontext":
                    new = "\tcall\tHIDDEN_JUMPTARGET(__gw_context_sigmask)"
                else:
                    saved = "%r9" if name == "swapcontext" else "%rdi"
                    new = (
                        f"\tpushq\t{saved}\n\tcfi_adjust_cfa_offset(8)\n"
                        "\tcall\tHIDDEN_JUMPTARGET(__gw_context_sigmask)\n"
                        f"\tpopq\t{saved}\n\tcfi_adjust_cfa_offset(-8)"
                    )
                new += "\n\tnegq\t%rax"
            else:
                old = (
                    "\tmov\tx3, "
                    + ("#_NSIG8" if name == "setcontext" else "_NSIG8")
                    + "\n\tmov\tx8, SYS_ify (rt_sigprocmask)\n\tsvc\t0"
                )
                new = (
                    "\tstp\tx9, x30, [sp, #-16]!\n"
                    "\tcfi_adjust_cfa_offset(16)\n"
                    "\tcfi_rel_offset(x30, 8)\n"
                    "\tbl\tHIDDEN_JUMPTARGET(__gw_context_sigmask)\n"
                    "\tldp\tx9, x30, [sp], #16\n"
                    "\tcfi_adjust_cfa_offset(-16)\n"
                    "\tcfi_restore(x30)\n\tneg\tx0, x0"
                )
            if text.count(old) != 1:
                raise SystemExit("Context-mask patch mismatch: " + str(path))
            path.write_text(text.replace(old, new))
