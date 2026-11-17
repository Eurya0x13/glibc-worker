/* Tail-call the owning libc capture. A C wrapper that returns after setjmp
 * would save a dead wrapper frame instead of the application checkpoint. */
#if defined(__x86_64__)
.text
.macro CAPTURE name, kind
.global \name
.type \name,@function
\name:
    push %rdi
    push %rsi
    sub $8, %rsp
    mov $\kind, %esi
    .if \kind == 3
    lea 32(%rsp), %rdx
    mov 24(%rsp), %rcx
    .endif
    call gw_nonlocal_capture
    add $8, %rsp
    pop %rsi
    pop %rdi
    jmp *%rax
.size \name,.-\name
.endm
#ifdef __ANDROID__
CAPTURE sigsetjmp, 0
#else
CAPTURE __sigsetjmp, 0
CAPTURE getcontext, 3
#endif
CAPTURE setjmp, 1
CAPTURE _setjmp, 2
#elif defined(__aarch64__)
.text
.macro CAPTURE name, kind
.global \name
.type \name,%function
\name:
    sub sp, sp, #32
    stp x0, x1, [sp]
    str x30, [sp, #16]
    mov w1, #\kind
    .if \kind == 3
    add x2, sp, #32
    mov x3, x30
    .endif
    bl gw_nonlocal_capture
    mov x9, x0
    ldp x0, x1, [sp]
    ldr x30, [sp, #16]
    add sp, sp, #32
    br x9
.size \name,.-\name
.endm
#ifdef __ANDROID__
CAPTURE sigsetjmp, 0
#else
CAPTURE __sigsetjmp, 0
CAPTURE getcontext, 3
#endif
CAPTURE setjmp, 1
CAPTURE _setjmp, 2
#else
#error Unsupported native checkpoint architecture
#endif
.section .note.GNU-stack,"",%progbits
