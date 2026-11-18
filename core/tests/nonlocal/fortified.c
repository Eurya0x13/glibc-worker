/* A separate translation unit verifies the public fortified libc spelling.
 * Do not compile the interposition definitions with this redirection. */
#undef _FORTIFY_SOURCE
#define _FORTIFY_SOURCE 2
#include <setjmp.h>

_Noreturn void fortified_jump(jmp_buf target, int value) {
  longjmp(target, value);
}
