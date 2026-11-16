#ifndef GW_BOOTSTRAP_RAW_SYSCALL_H
#define GW_BOOTSTRAP_RAW_SYSCALL_H

/* Only for SIGSYS diagnostics before/after Guest TLS initialization.
 * No libc calls, errno access, stack protector or thread-pointer dependency. */
/* Returns the raw kernel value: a negative result is -errno, and errno
 * itself is never read or written. */
static __attribute__((always_inline)) inline long gw_raw3(long nr, long a,
                                                          long b, long c) {
#if defined(__x86_64__)
  long result;
  __asm__ volatile("syscall"
                   : "=a"(result)
                   : "a"(nr), "D"(a), "S"(b), "d"(c)
                   : "rcx", "r11", "memory");
  return result;
#elif defined(__aarch64__)
  register long x0 __asm__("x0") = a;
  register long x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0"
                   : "+r"(x0)
                   : "r"(x1), "r"(x2), "r"(x8)
                   : "memory", "cc");
  return x0;
#else
#error Unsupported raw syscall ABI
#endif
}

/* futex WAIT also needs an explicit NULL timeout; leaving the fourth kernel
 * argument unspecified would turn a blocking wait into an arbitrary pointer. */
static __attribute__((always_inline)) inline long gw_raw4(long nr, long a,
                                                          long b, long c,
                                                          long d) {
#if defined(__x86_64__)
  register long fourth __asm__("r10") = d;
  long result;
  __asm__ volatile("syscall"
                   : "=a"(result)
                   : "a"(nr), "D"(a), "S"(b), "d"(c), "r"(fourth)
                   : "rcx", "r11", "memory");
  return result;
#elif defined(__aarch64__)
  register long x0 __asm__("x0") = a;
  register long x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c;
  register long x3 __asm__("x3") = d;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0"
                   : "+r"(x0)
                   : "r"(x1), "r"(x2), "r"(x3), "r"(x8)
                   : "memory", "cc");
  return x0;
#endif
}
#endif
