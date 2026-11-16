/* Included by the owning glibc's sigjmp.c after setjmpP.h.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Capture runs after native assembly saved its caller, never on a wrapper's
 * soon-to-return frame. Restore runs before libc's own cleanup/mask/checked
 * machine restore. The callback only coordinates bridge-owned scopes; it
 * never decodes or edits an opaque native jump buffer.
 * https://github.com/bminor/glibc/blob/glibc-2.41/setjmp/sigjmp.c
 * https://github.com/bminor/glibc/blob/glibc-2.41/setjmp/longjmp.c
 * https://github.com/bminor/glibc/blob/glibc-2.41/debug/longjmp_chk.c
 */
static void (*gw_jump_hook)(void *, int, int);

int __gw_domain_jump_hook(void (*hook)(void *, int, int));
void __gw_domain_jump_event(void *, int, int) attribute_hidden;

int __gw_domain_jump_hook(void (*hook)(void *, int, int)) {
  if (hook == NULL || gw_jump_hook != NULL) {
    return EINVAL;
  }
  gw_jump_hook = hook;
  return 0;
}

void __gw_domain_jump_event(void *buffer, int value, int restore) {
  if (gw_jump_hook != NULL) {
    gw_jump_hook(buffer, value, restore);
  }
}
