/* Kernel signal frame stays on the Host-owned alternate signal stack.
 * Only this TLS-free entry may call the pre-restore registry lookup. */
#if defined(__x86_64__)
.text
.global domain_signal_entry
.type domain_signal_entry,@function
domain_signal_entry:
    push %rbx
    sub $48, %rsp
    mov %rdi, 0(%rsp)
    mov %rsi, 8(%rsp)
    mov %rdx, 16(%rsp)
    mov %rcx, 32(%rsp)
    mov $158, %eax
    mov $0x1003, %edi
    lea 24(%rsp), %rsi
    syscall
    test %rax, %rax
    jnz .Lbad_signal
    mov 24(%rsp), %rdi
    call domain_signal_lookup
    cmp $-1, %rax
    je .Lbad_signal
    mov %rax, %rbx
    test %rax, %rax
#ifdef GW_DOMAIN_CHAIN
    jz .Ldispatch_signal
#else
    jz .Lbad_signal
#endif
    mov 0(%rbx), %rsi
    mov $158, %eax
    mov $0x1002, %edi
    syscall
    test %rax, %rax
    jnz .Lbad_signal
.Ldispatch_signal:
    mov 0(%rsp), %rdi
    mov 8(%rsp), %rsi
    mov 16(%rsp), %rdx
    mov %rbx, %rcx
    mov 24(%rsp), %r8
    mov 32(%rsp), %r9
    call domain_signal_dispatch
    mov 24(%rsp), %rsi
    mov $158, %eax
    mov $0x1002, %edi
    syscall
    test %rax, %rax
    jnz .Lbad_signal
    add $48, %rsp
    pop %rbx
    ret
.Lbad_signal:
    mov $231, %eax
    mov $96, %edi
    syscall
    ud2
.size domain_signal_entry,.-domain_signal_entry
.global domain_fault
.type domain_fault,@function
domain_fault:
.global domain_fault_instruction
domain_fault_instruction:
    mov (%rdi), %eax
.global domain_fault_resume
domain_fault_resume:
    ret
.size domain_fault,.-domain_fault
#elif defined(__aarch64__)
.text
.global domain_signal_entry
.type domain_signal_entry,%function
domain_signal_entry:
    sub sp, sp, #80
    stp x19, x30, [sp]
    mrs x9, tpidr_el0
    stp x9, x18, [sp, #16]
    stp x0, x1, [sp, #32]
    str x2, [sp, #48]
    str x3, [sp, #56]
    ldr x0, [sp, #16]
    bl domain_signal_lookup
    cmn x0, #1
    b.eq .Lbad_signal
    mov x19, x0
#ifdef GW_DOMAIN_CHAIN
    cbz x0, .Ldispatch_signal
#else
    cbz x0, .Lbad_signal
#endif
    ldp x9, x18, [x19]
    msr tpidr_el0, x9
.Ldispatch_signal:
    ldp x0, x1, [sp, #32]
    ldr x2, [sp, #48]
    mov x3, x19
    ldr x4, [sp, #16]
    ldr x5, [sp, #56]
    bl domain_signal_dispatch
    ldp x9, x18, [sp, #16]
    msr tpidr_el0, x9
    ldp x19, x30, [sp]
    add sp, sp, #80
    ret
.Lbad_signal:
    mov x0, #96
    mov x8, #94
    svc #0
    brk #0
.size domain_signal_entry,.-domain_signal_entry
.global domain_fault
.type domain_fault,%function
domain_fault:
.global domain_fault_instruction
domain_fault_instruction:
    ldr w9, [x0]
.global domain_fault_resume
domain_fault_resume:
    ret
.size domain_fault,.-domain_fault
#endif
.section .note.GNU-stack,"",%progbits
