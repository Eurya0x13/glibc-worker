/* Enter a handler on the selected domain's stack while delivery is blocked.
 * The caller owns both stacks until this call returns or is abandoned through
 * the owning domain's native nonlocal restore. A zero top keeps this stack. */
#if defined(__x86_64__)
.text
.global domain_stack_call
.type domain_stack_call,@function
domain_stack_call:
    .cfi_startproc
    push %r12
    .cfi_def_cfa_offset 16
    .cfi_offset %r12, -16
    mov %rsp, %r12
    .cfi_def_cfa %r12, 16
    test %rcx, %rcx
    jz .Lstack_unrecorded
    mov %rsp, (%rcx)
.Lstack_unrecorded:
    mov %rdi, %rax
    mov %rsi, %rdi
    test %rdx, %rdx
    jz .Lstack_ready
    mov %rdx, %rsp
    and $-16, %rsp
.Lstack_ready:
    call *%rax
    mov %r12, %rsp
    .cfi_def_cfa %rsp, 16
    pop %r12
    .cfi_def_cfa_offset 8
    .cfi_restore %r12
    ret
    .cfi_endproc
.size domain_stack_call,.-domain_stack_call
#elif defined(__aarch64__)
.text
.global domain_stack_call
.type domain_stack_call,%function
domain_stack_call:
    .cfi_startproc
    stp x19, x30, [sp, #-16]!
    .cfi_def_cfa_offset 16
    .cfi_offset x19, -16
    .cfi_offset x30, -8
    mov x19, sp
    .cfi_def_cfa x19, 16
    cbz x3, .Lstack_unrecorded
    str x19, [x3]
.Lstack_unrecorded:
    mov x9, x0
    mov x0, x1
    cbz x2, .Lstack_ready
    and x2, x2, #-16
    mov sp, x2
.Lstack_ready:
    blr x9
    mov sp, x19
    .cfi_def_cfa sp, 16
    ldp x19, x30, [sp], #16
    .cfi_def_cfa_offset 0
    .cfi_restore x19
    .cfi_restore x30
    ret
    .cfi_endproc
.size domain_stack_call,.-domain_stack_call
#else
#error Unsupported domain stack architecture
#endif
.section .note.GNU-stack,"",%progbits
