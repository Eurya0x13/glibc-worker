/* A single, identifiable syscall boundary for public LP64 atomic mask waits.
 * The first word of the stable wait row receives the interrupted stack value.
 * No libc, TLS, cancellation record or automatic-frame pointer is published.
 */
.text
.global gw_signal_wait_syscall
.global gw_signal_wait_resume
.type gw_signal_wait_syscall,%function
#if defined(__x86_64__)
gw_signal_wait_syscall:
    .cfi_startproc
    endbr64
    mov %rsp, (%rdi)
    mov %rsi, %rax
    mov %rdx, %rdi
    mov %rcx, %rsi
    mov %r8, %rdx
    mov %r9, %r10
    mov 8(%rsp), %r8
    mov 16(%rsp), %r9
    syscall
gw_signal_wait_resume:
    ret
    .cfi_endproc
#elif defined(__aarch64__)
gw_signal_wait_syscall:
    .cfi_startproc
    .inst 0xd503245f
    mov x9, sp
    str x9, [x0]
    mov x8, x1
    mov x0, x2
    mov x1, x3
    mov x2, x4
    mov x3, x5
    mov x4, x6
    mov x5, x7
    svc #0
gw_signal_wait_resume:
    ret
    .cfi_endproc
#else
#error Unsupported atomic mask-wait architecture
#endif
.size gw_signal_wait_syscall,.-gw_signal_wait_syscall
.section .note.GNU-stack,"",%progbits
