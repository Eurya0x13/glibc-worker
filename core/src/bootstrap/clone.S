#if defined(__x86_64__)
.text
.global launch_guest
.type launch_guest,@function
/* long launch_guest(unsigned long flags, void *initial_sp, void *entry);
   Child stack has no C return frame: enter ld.so using its startup ABI. */
launch_guest:
    mov %rdx, %r9
    xor %edx, %edx
    xor %r10d, %r10d
    xor %r8d, %r8d
    mov $56, %eax
    syscall
    test %rax, %rax
    jnz 1f
    /* Remove inherited bionic/glibc Host FS; ld.so establishes Guest FS. */
    mov $158, %eax
    mov $0x1002, %edi
    xor %esi, %esi
    syscall
    test %rax, %rax
    jnz 2f
    xor %ebp, %ebp
    xor %edx, %edx
    jmp *%r9
1:  ret
2:  mov $60, %eax
    mov $90, %edi
    syscall
.size launch_guest,.-launch_guest
#elif defined(__aarch64__)
.text
.global launch_guest
.type launch_guest,%function
launch_guest:
    mov x9, x2
    mov x2, xzr
    mov x3, xzr
    mov x4, xzr
    mov x8, #220
    svc #0
    cbnz x0, 1f
    msr tpidr_el0, xzr
    mov x29, xzr
    mov x30, xzr
    br x9
1:  ret
.size launch_guest,.-launch_guest
#else
#error unsupported architecture
#endif
.section .note.GNU-stack,"",%progbits
