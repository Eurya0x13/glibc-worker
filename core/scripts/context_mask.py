"""Route the mask operation and report native context lifetime events."""

from pathlib import Path


def defer_delivery_until_target_stack(arch: str, text: str) -> str:
    if arch == "aarch64":
        points = ("\tldr     x16, [x0, oPC]",)
        call = (
            "\tadrp\tx1, :gottprel:__gw_context_deferred_target\n"
            "\tldr\tx1, [x1, #:gottprel_lo12:__gw_context_deferred_target]\n"
            "\tmrs\tx2, tpidr_el0\n"
            "\tldr\tx1, [x2, x1]\n"
            "\tcbz\tx1, 9f\n"
            "\tstp\tx0, x30, [sp, #-16]!\n"
            "\tbl\tHIDDEN_JUMPTARGET(__gw_context_complete)\n"
            "\tldp\tx0, x30, [sp], #16\n"
            "9:\n"
        )
    else:
        points = (
            "L(skip_unwind_shadow_stack):\n",
            "L(no_shstk):\n#endif\n",
        )
        call = (
            "\tmovq\t__gw_context_deferred_target@gottpoff(%rip), %r10\n"
            "\tcmpq\t$0, %fs:(%r10)\n"
            "\tje\t9f\n"
            "\tmovq\t%rsp, %r10\n"
            "\tleaq\t-128(%rsp), %rsp\n"
            "\tandq\t$-16, %rsp\n"
            "\tsubq\t$16, %rsp\n"
            "\tmovq\t%r10, (%rsp)\n"
            "\tmovq\t%rdx, 8(%rsp)\n"
            "\tcall\tHIDDEN_JUMPTARGET(__gw_context_complete)\n"
            "\tmovq\t8(%rsp), %rdx\n"
            "\tmovq\t(%rsp), %rsp\n"
            "9:\n"
        )

    for point in points:
        if text.count(point) != 1:
            raise SystemExit("Context-restore stack patch mismatch: " + arch)
        replacement = point + call if point.endswith("\n") else call + point
        text = text.replace(point, replacement)
    return text


def preserve_swap_error_state(arch: str, text: str) -> str:
    # The new preflight can fail with valid context pointers. Restore the
    # native caller state even when the target never runs.
    if arch == "x86_64":
        old = (
            "\tjae\tSYSCALL_ERROR_LABEL\t/* Jump to error handler if error.  */"
        )
        new = (
            "\tjb\tL(gw_mask_ok)\n"
            "\tmovq\toR12(%r9), %r12\n"
            "\tfldenv\toFPREGSMEM(%r9)\n"
            "\tjmp\tSYSCALL_ERROR_LABEL\n"
            "L(gw_mask_ok):"
        )
    else:
        # x30 may now point to the saved-context return stub. Preserve the
        # caller's real return address for the syscall error path as well.
        old = "\tstp\txzr, x30, [x0, oX0 +  0 * SZREG]"
        new = old + "\n\tmov\tx10, x30"
    if text.count(old) != 1:
        raise SystemExit("Context-swap error patch mismatch: " + arch)
    return text.replace(old, new)


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
                new = "\txorl\t%ecx, %ecx\n" + new + "\n\tnegq\t%rax"
            else:
                old = (
                    "\tmov\tx3, "
                    + ("#_NSIG8" if name == "setcontext" else "_NSIG8")
                    + "\n\tmov\tx8, SYS_ify (rt_sigprocmask)\n\tsvc\t0"
                )
                target = "x9" if name == "swapcontext" else "xzr"
                link = "x10" if name == "swapcontext" else "x30"
                new = (
                    f"\tstp\tx9, {link}, [sp, #-16]!\n"
                    "\tcfi_adjust_cfa_offset(16)\n"
                    "\tcfi_rel_offset(x30, 8)\n"
                    f"\tmov\tx3, {target}\n"
                    "\tbl\tHIDDEN_JUMPTARGET(__gw_context_sigmask)\n"
                    "\tldp\tx9, x30, [sp], #16\n"
                    "\tcfi_adjust_cfa_offset(-16)\n"
                    "\tcfi_restore(x30)\n\tneg\tx0, x0"
                )
            if text.count(old) != 1:
                raise SystemExit("Context-mask patch mismatch: " + str(path))
            text = text.replace(old, new)
            if name == "swapcontext":
                text = preserve_swap_error_state(arch, text)
            if name == "setcontext" or (
                arch == "x86_64" and name == "swapcontext"
            ):
                text = defer_delivery_until_target_stack(arch, text)
            path.write_text(text)

        path = linux / arch / "makecontext.c"
        text = path.read_text()
        include = "#include <ucontext.h>"
        finish = "  va_end (ap);"
        if text.count(include) != 1 or text.count(finish) != 1:
            raise SystemExit(
                "Context-construction patch mismatch: " + str(path)
            )
        text = text.replace(
            include,
            include
            + "\n\nextern void __gw_context_constructed (ucontext_t *) attribute_hidden;",
        )
        path.write_text(
            text.replace(finish, finish + "\n  __gw_context_constructed (ucp);")
        )
