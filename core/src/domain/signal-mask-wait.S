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

/* Native sleep/other non-mask waits need a bounded unmask/entry gateway.
 * The router recognizes the whole unmasked setup window by PC and stable SP.
 * A pre-entry signal skips the syscall with EINTR; an actual return retains
 * the kernel's result and captures valid handler mask edits before C resumes.
 * All pointers published to the router refer to stable process-owned rows. */
.global gw_signal_native_wait_syscall
.global gw_signal_native_unmasked
.global gw_signal_native_resume
.global gw_signal_native_rearm
.global gw_signal_native_rearm_blocked
.type gw_signal_native_wait_syscall,%function
#if defined(__x86_64__)
gw_signal_native_wait_syscall:
    .cfi_startproc
    endbr64
    push %rbx
    .cfi_adjust_cfa_offset 8
    .cfi_offset %rbx, -16
    sub $80, %rsp
    .cfi_adjust_cfa_offset 80
    mov %rdi, %rbx
    mov %rsp, (%rbx)
    mov %rsi, 0(%rsp)
    mov %rdx, 8(%rsp)
    mov %rcx, 16(%rsp)
    mov %r8, 24(%rsp)
    mov %r9, 32(%rsp)
    mov 96(%rsp), %rax
    mov %rax, 40(%rsp)
    mov 104(%rsp), %rax
    mov %rax, 48(%rsp)
    movq $-1, 56(%rsp)
.Lnative_unmask:
    mov $14, %eax
    mov $2, %edi
    lea 32(%rbx), %rsi
    xor %edx, %edx
    mov $8, %r10d
    syscall
gw_signal_native_unmasked:
    test %rax, %rax
    jnz .Lnative_out
    movl $1, 52(%rbx)
    mov 0(%rsp), %rax
    mov 8(%rsp), %rdi
    mov 16(%rsp), %rsi
    mov 24(%rsp), %rdx
    mov 32(%rsp), %r10
    mov 40(%rsp), %r8
    mov 48(%rsp), %r9
    syscall
gw_signal_native_resume:
    mov %rax, 64(%rsp)
    mov $14, %eax
    mov $2, %edi
    lea 56(%rsp), %rsi
    lea 40(%rbx), %rdx
    mov $8, %r10d
    syscall
    test %rax, %rax
    jnz .Lnative_fatal
    movl $1, 48(%rbx)
    mov 64(%rsp), %rax
.Lnative_out:
    add $80, %rsp
    .cfi_adjust_cfa_offset -80
    pop %rbx
    .cfi_adjust_cfa_offset -8
    .cfi_restore %rbx
    ret
gw_signal_native_rearm:
    /* This branch retains the original prologue's CFA/save locations. */
    .cfi_def_cfa_offset 96
    .cfi_offset %rbx, -16
    mov $14, %eax
    mov $2, %edi
    lea 56(%rsp), %rsi
    lea 32(%rbx), %rdx
    mov $8, %r10d
    syscall
gw_signal_native_rearm_blocked:
    test %rax, %rax
    jnz .Lnative_fatal
    mov %rbx, %rdi
    call gw_signal_native_wait_rearm
    jmp .Lnative_unmask
.Lnative_fatal:
    mov $231, %eax
    mov $125, %edi
    syscall
    ud2
    .cfi_endproc
#elif defined(__aarch64__)
gw_signal_native_wait_syscall:
    .cfi_startproc
    .inst 0xd503245f
    stp x19, x30, [sp, #-96]!
    .cfi_adjust_cfa_offset 96
    .cfi_offset x19, -96
    .cfi_offset x30, -88
    mov x19, x0
    mov x9, sp
    str x9, [x19]
    stp x1, x2, [sp, #16]
    stp x3, x4, [sp, #32]
    stp x5, x6, [sp, #48]
    str x7, [sp, #64]
    mov x9, #-1
    str x9, [sp, #72]
.Lnative_unmask:
    mov x0, #2
    add x1, x19, #32
    mov x2, #0
    mov x3, #8
    mov x8, #135
    svc #0
gw_signal_native_unmasked:
    cbnz x0, .Lnative_out
    mov w9, #1
    str w9, [x19, #52]
    ldp x8, x0, [sp, #16]
    ldp x1, x2, [sp, #32]
    ldp x3, x4, [sp, #48]
    ldr x5, [sp, #64]
    svc #0
gw_signal_native_resume:
    str x0, [sp, #80]
    mov x0, #2
    add x1, sp, #72
    add x2, x19, #40
    mov x3, #8
    mov x8, #135
    svc #0
    cbnz x0, .Lnative_fatal
    mov w9, #1
    str w9, [x19, #48]
    ldr x0, [sp, #80]
.Lnative_out:
    ldp x19, x30, [sp], #96
    .cfi_adjust_cfa_offset -96
    .cfi_restore x19
    .cfi_restore x30
    ret
gw_signal_native_rearm:
    .cfi_def_cfa_offset 96
    .cfi_offset x19, -96
    .cfi_offset x30, -88
    mov x0, #2
    add x1, sp, #72
    add x2, x19, #32
    mov x3, #8
    mov x8, #135
    svc #0
gw_signal_native_rearm_blocked:
    cbnz x0, .Lnative_fatal
    mov x0, x19
    bl gw_signal_native_wait_rearm
    b .Lnative_unmask
.Lnative_fatal:
    mov x0, #125
    mov x8, #94
    svc #0
    brk #0
    .cfi_endproc
#endif
.size gw_signal_native_wait_syscall,.-gw_signal_native_wait_syscall
.section .note.GNU-stack,"",%progbits
