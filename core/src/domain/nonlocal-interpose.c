#include "nonlocal.h"

int gw_nonlocal_interposed(void) {
  return 1;
}

_Noreturn void siglongjmp(sigjmp_buf buffer, int value) {
  gw_nonlocal_jump(buffer, value, 0);
}

_Noreturn void longjmp(jmp_buf buffer, int value) {
  gw_nonlocal_jump(buffer, value, 1);
}

_Noreturn void _longjmp(jmp_buf buffer, int value) {
  gw_nonlocal_jump(buffer, value, 2);
}

#ifdef __GLIBC__
_Noreturn void __longjmp_chk(jmp_buf buffer, int value) {
  gw_nonlocal_jump(buffer, value, 3);
}
#endif

#ifndef __ANDROID__
int setcontext(const ucontext_t *context) {
  return gw_nonlocal_setcontext(context);
}
#endif
