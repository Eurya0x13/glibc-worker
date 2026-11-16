/* Preserve an optional vararg word without evaluating an absent C vararg. */
.text
.globl fcntl
.globl fcntl64
.type fcntl, %function
.type fcntl64, %function
fcntl:
fcntl64:
#if defined(__x86_64__)
  jmp signal_fd_fcntl
#elif defined(__aarch64__)
  b signal_fd_fcntl
#else
#error Unsupported descriptor ABI
#endif
.size fcntl, .-fcntl
.size fcntl64, .-fcntl64
.section .note.GNU-stack,"",%progbits
