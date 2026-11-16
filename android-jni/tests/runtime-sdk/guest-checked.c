#undef _FORTIFY_SOURCE
#define _FORTIFY_SOURCE 2
#include <setjmp.h>

/* A separate optimized translation unit must really bind __longjmp_chk. */
_Noreturn void sdk_guest_checked_jump(void *buffer, int value) {
  siglongjmp(*(sigjmp_buf *)buffer, value);
}
