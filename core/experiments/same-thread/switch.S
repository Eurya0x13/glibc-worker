/* Cooperative call boundary only. No unwinding, signal delivery or migration
 * through this window is supported. Each suspended stack retains its frames.
 * TLS accesses and stack protectors must never be emitted in this routine. */
#if defined(__x86_64__)
.text
.global domain_swap
.type domain_swap,@function
domain_swap:
    mov %rsp, 0(%rdi)
    lea .Lresume(%rip), %rax
    mov %rax, 16(%rdi)
    mov %rbx, 24(%rdi)
    mov %rbp, 32(%rdi)
    mov %r12, 40(%rdi)
    mov %r13, 48(%rdi)
    mov %r14, 56(%rdi)
    mov %r15, 64(%rdi)
    stmxcsr 72(%rdi)
    fnstcw 80(%rdi)
    mov %rdi, %r8
    mov %rsi, %r9
    /* Baseline intentionally uses syscalls, not assumed FSGSBASE support. */
    mov $158, %eax
    mov $0x1003, %edi
    lea 8(%r8), %rsi
    syscall
    test %rax, %rax
    jnz .Lfatal
    mov $158, %eax
    mov $0x1002, %edi
    mov 8(%r9), %rsi
    syscall
    test %rax, %rax
    jnz .Lfatal
    mov 24(%r9), %rbx
    mov 32(%r9), %rbp
    mov 40(%r9), %r12
    mov 48(%r9), %r13
    mov 56(%r9), %r14
    mov 64(%r9), %r15
    ldmxcsr 72(%r9)
    fldcw 80(%r9)
    mov 0(%r9), %rsp
    xor %edx, %edx
    jmp *16(%r9)
.Lresume:
    ret
.Lfatal:
    mov $231, %eax
    mov $90, %edi
    syscall
    ud2
.size domain_swap,.-domain_swap
#elif defined(__aarch64__)
.text
.global domain_swap
.type domain_swap,%function
domain_swap:
    mov x9, sp
    mrs x10, tpidr_el0
    stp x9, x10, [x0]
    adr x9, .Lresume
    str x9, [x0, #16]
    stp x19, x20, [x0, #24]
    stp x21, x22, [x0, #40]
    stp x23, x24, [x0, #56]
    stp x25, x26, [x0, #72]
    stp x27, x28, [x0, #88]
    stp x29, x30, [x0, #104]
    str x18, [x0, #120]
    stp d8, d9, [x0, #128]
    stp d10, d11, [x0, #144]
    stp d12, d13, [x0, #160]
    stp d14, d15, [x0, #176]
    mrs x9, fpcr
    mrs x10, fpsr
    stp x9, x10, [x0, #192]
    ldp x9, x10, [x1]
    msr tpidr_el0, x10
    mov sp, x9
    ldp x19, x20, [x1, #24]
    ldp x21, x22, [x1, #40]
    ldp x23, x24, [x1, #56]
    ldp x25, x26, [x1, #72]
    ldp x27, x28, [x1, #88]
    ldp x29, x30, [x1, #104]
    ldr x18, [x1, #120]
    ldp d8, d9, [x1, #128]
    ldp d10, d11, [x1, #144]
    ldp d12, d13, [x1, #160]
    ldp d14, d15, [x1, #176]
    ldp x9, x10, [x1, #192]
    msr fpcr, x9
    msr fpsr, x10
    ldr x9, [x1, #16]
    br x9
.Lresume:
    ret
.size domain_swap,.-domain_swap
#else
#error Unsupported domain-switch architecture
#endif
.section .note.GNU-stack,"",%progbits
