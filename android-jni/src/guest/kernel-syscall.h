#ifndef GW_GUEST_KERNEL_SYSCALL_H
#define GW_GUEST_KERNEL_SYSCALL_H
#ifndef __ASSEMBLER__
#include <unistd.h>

typedef long (*gw_kernel_syscall)(long, ...);
extern gw_kernel_syscall gw_guest_kernel_call;

/* Runtime-internal kernel operations bypass the public GNU syscall
 * dispatcher, including operations on private backing descriptors. */
#define syscall gw_guest_kernel_call
#endif
#endif
