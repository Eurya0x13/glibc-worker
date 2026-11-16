/* Enter a handler on the selected domain's stack while delivery is blocked.
 * The caller owns both stacks until this call returns or is abandoned through
 * the owning domain's native nonlocal restore. A zero top keeps this stack. */
#if defined(__x86_64__)
.text
.global domain_stack_call
.type domain_stack_call,@function
domain_stack_call:
    push %r12
    mov %rsp, %r12
    mov %rdi, %rax
    mov %rsi, %rdi
    test %rdx, %rdx
    jz .Lstack_ready
    mov %rdx, %rsp
    and $-16, %rsp
.Lstack_ready:
    call *%rax
    mov %r12, %rsp
    pop %r12
    ret
.size domain_stack_call,.-domain_stack_call
#elif defined(__aarch64__)
.text
.global domain_stack_call
.type domain_stack_call,%function
domain_stack_call:
    stp x19, x30, [sp, #-16]!
    mov x19, sp
    mov x9, x0
    mov x0, x1
    cbz x2, .Lstack_ready
    and x2, x2, #-16
    mov sp, x2
.Lstack_ready:
    blr x9
    mov sp, x19
    ldp x19, x30, [sp], #16
    ret
.size domain_stack_call,.-domain_stack_call
#else
#error Unsupported domain stack architecture
#endif
.section .note.GNU-stack,"",%progbits
