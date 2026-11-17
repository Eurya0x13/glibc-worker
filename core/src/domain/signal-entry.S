/* The kernel frame belongs to the executing domain's configured signal stack.
 * Block delivery through TP/stack transitions; the selected handler restores
 * its effective logical mask and the kernel owns final sigreturn restoration.
 * Only this TLS-free entry may call the pre-restore registry lookup. */
#if defined(__x86_64__)
.text
.global domain_signal_entry
.type domain_signal_entry,@function
domain_signal_entry:
    push %rbx
    sub $64, %rsp
    mov %rdi, 0(%rsp)
    mov %rsi, 8(%rsp)
    mov %rdx, 16(%rsp)
    mov %rcx, 32(%rsp)
#ifdef GW_DOMAIN_CHAIN
    movq $-1, 56(%rsp)
    mov $14, %eax
    mov $2, %edi
    lea 56(%rsp), %rsi
    lea 48(%rsp), %rdx
    mov $8, %r10d
    syscall
    test %rax, %rax
    jnz .Lbad_signal
#endif
    mov $158, %eax
    mov $0x1003, %edi
    lea 24(%rsp), %rsi
    syscall
    test %rax, %rax
    jnz .Lbad_signal
    mov 24(%rsp), %rdi
    call domain_signal_lookup
    cmp $-1, %rax
#ifdef GW_DOMAIN_CHAIN
    je .Lguest_thread_signal
#else
    je .Lbad_signal
#endif
    mov %rax, %rbx
    test %rax, %rax
#ifdef GW_DOMAIN_CHAIN
    jz .Ldispatch_signal
#else
    jz .Lbad_signal
#endif
#ifdef GW_DOMAIN_CHAIN
    mov 24(%rsp), %rax
    cmp 0(%rbx), %rax
    je .Lhost_tp
    mov 0(%rsp), %edi
    call domain_guest_notify_select
    test %rax, %rax
    jnz .Ltail_guest
.Lhost_tp:
#endif
    mov 0(%rbx), %rsi
    mov $158, %eax
    mov $0x1002, %edi
    syscall
    test %rax, %rax
    jnz .Lbad_signal
    /* A nonlocal exit skips kernel sigreturn's floating-point restoration.
     * Host handlers entered from Guest need the suspended Host control state,
     * not the kernel's initial signal-handler environment. */
    mov 24(%rsp), %rax
    cmp 0(%rbx), %rax
    je .Ldispatch_signal
    mov 16(%rbx), %rax
    ldmxcsr 72(%rax)
    fldcw 80(%rax)
.Ldispatch_signal:
    mov 0(%rsp), %rdi
    mov 8(%rsp), %rsi
    mov 16(%rsp), %rdx
    mov %rbx, %rcx
    mov 24(%rsp), %r8
    mov 32(%rsp), %r9
#ifdef GW_DOMAIN_CHAIN
    mov 48(%rsp), %rax
#else
    xor %eax, %eax
#endif
    /* Seventh C argument: actual kernel handler-entry mask. sigsuspend and
     * mask-wait frames save their pre-wait mask in ucontext instead. */
    mov %rax, 0(%rsp)
    call domain_signal_dispatch
.Lrestore_signal:
    mov 24(%rsp), %rsi
    mov $158, %eax
    mov $0x1002, %edi
    syscall
    test %rax, %rax
    jnz .Lbad_signal
    add $64, %rsp
    pop %rbx
    ret
#ifdef GW_DOMAIN_CHAIN
.Lguest_thread_signal:
    mov 0(%rsp), %rdi
    mov 8(%rsp), %rsi
    call domain_guest_thread_select
    test %rax, %rax
    jz .Lrelay_guest_thread
.Ltail_guest:
    mov %rax, %r11
    mov 0(%rsp), %rdi
    mov 8(%rsp), %rsi
    mov 16(%rsp), %rdx
    add $64, %rsp
    pop %rbx
    jmp *%r11
.Lrelay_guest_thread:
    mov 0(%rsp), %rdi
    mov 8(%rsp), %rsi
    mov 16(%rsp), %rdx
    mov 32(%rsp), %rcx
    mov 48(%rsp), %r8
    call domain_guest_thread_dispatch
    jmp .Lrestore_signal
#endif
.Lbad_signal:
    mov $231, %eax
    mov $96, %edi
    syscall
    ud2
.size domain_signal_entry,.-domain_signal_entry
.global domain_guest_signal_call
.type domain_guest_signal_call,@function
domain_guest_signal_call:
    push %r12
    sub $32, %rsp
    mov %rdi, %r12
    stmxcsr 8(%rsp)
    fnstcw 12(%rsp)
    mov $158, %eax
    mov $0x1003, %edi
    lea 0(%rsp), %rsi
    syscall
    test %rax, %rax
    jnz .Lbad_signal
    mov $158, %eax
    mov $0x1002, %edi
    mov 8(%r12), %rsi
    syscall
    test %rax, %rax
    jnz .Lbad_signal
    ldmxcsr 24(%r12)
    fldcw 32(%r12)
    mov 40(%r12), %edi
    mov 48(%r12), %rsi
    mov 56(%r12), %rdx
    call *0(%r12)
    mov %eax, 16(%rsp)
    mov $158, %eax
    mov $0x1002, %edi
    mov 0(%rsp), %rsi
    syscall
    test %rax, %rax
    jnz .Lbad_signal
    ldmxcsr 8(%rsp)
    fldcw 12(%rsp)
    mov 16(%rsp), %eax
    add $32, %rsp
    pop %r12
    ret
.size domain_guest_signal_call,.-domain_guest_signal_call
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
#ifdef GW_DOMAIN_CHAIN
    mov x9, #-1
    str x9, [sp, #72]
    mov x0, #2
    add x1, sp, #72
    add x2, sp, #64
    mov x3, #8
    mov x8, #135
    svc #0
    cbnz x0, .Lbad_signal
#endif
    ldr x0, [sp, #16]
    bl domain_signal_lookup
    cmn x0, #1
#ifdef GW_DOMAIN_CHAIN
    b.eq .Lguest_thread_signal
#else
    b.eq .Lbad_signal
#endif
    mov x19, x0
#ifdef GW_DOMAIN_CHAIN
    cbz x0, .Ldispatch_signal
#else
    cbz x0, .Lbad_signal
#endif
#ifdef GW_DOMAIN_CHAIN
    ldr x9, [x19]
    ldr x10, [sp, #16]
    cmp x9, x10
    b.eq .Lhost_tp
    ldr w0, [sp, #32]
    bl domain_guest_notify_select
    cbnz x0, .Ltail_guest
.Lhost_tp:
#endif
    ldp x9, x18, [x19]
    msr tpidr_el0, x9
    ldr x10, [sp, #16]
    cmp x9, x10
    b.eq .Ldispatch_signal
    ldr x9, [x19, #16]
    ldp x10, x11, [x9, #192]
    msr fpcr, x10
    msr fpsr, x11
.Ldispatch_signal:
    ldp x0, x1, [sp, #32]
    ldr x2, [sp, #48]
    mov x3, x19
    ldr x4, [sp, #16]
    ldr x5, [sp, #56]
#ifdef GW_DOMAIN_CHAIN
    ldr x6, [sp, #64]
#else
    mov x6, #0
#endif
    bl domain_signal_dispatch
.Lrestore_signal:
    ldp x9, x18, [sp, #16]
    msr tpidr_el0, x9
    ldp x19, x30, [sp]
    add sp, sp, #80
    ret
#ifdef GW_DOMAIN_CHAIN
.Lguest_thread_signal:
    ldp x0, x1, [sp, #32]
    bl domain_guest_thread_select
    cbz x0, .Lrelay_guest_thread
.Ltail_guest:
    mov x16, x0
    ldp x0, x1, [sp, #32]
    ldr x2, [sp, #48]
    ldp x19, x30, [sp]
    add sp, sp, #80
    br x16
.Lrelay_guest_thread:
    ldp x0, x1, [sp, #32]
    ldr x2, [sp, #48]
    ldr x3, [sp, #56]
    ldr x4, [sp, #64]
    bl domain_guest_thread_dispatch
    b .Lrestore_signal
#endif
.Lbad_signal:
    mov x0, #96
    mov x8, #94
    svc #0
    brk #0
.size domain_signal_entry,.-domain_signal_entry
.global domain_guest_signal_call
.type domain_guest_signal_call,%function
domain_guest_signal_call:
    stp x19, x30, [sp, #-64]!
    mov x19, x0
    mrs x9, tpidr_el0
    stp x9, x18, [sp, #16]
    mrs x9, fpcr
    mrs x10, fpsr
    stp x9, x10, [sp, #32]
    ldp x9, x18, [x19, #8]
    msr tpidr_el0, x9
    ldp x9, x10, [x19, #24]
    msr fpcr, x9
    msr fpsr, x10
    ldr w0, [x19, #40]
    ldp x1, x2, [x19, #48]
    ldr x9, [x19]
    blr x9
    str w0, [sp, #48]
    ldp x9, x18, [sp, #16]
    msr tpidr_el0, x9
    ldp x9, x10, [sp, #32]
    msr fpcr, x9
    msr fpsr, x10
    ldr w0, [sp, #48]
    ldp x19, x30, [sp], #64
    ret
.size domain_guest_signal_call,.-domain_guest_signal_call
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
