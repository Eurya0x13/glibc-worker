#if defined(__x86_64__)
.text
.global domain_start
.type domain_start,@function
domain_start:
    mov %r12, %rdi
    call *%r13
    ud2
.size domain_start,.-domain_start
#elif defined(__aarch64__)
.text
.global domain_start
.type domain_start,%function
domain_start:
    mov x0, x19
    blr x20
    brk #0
.size domain_start,.-domain_start
#endif
.section .note.GNU-stack,"",%progbits
