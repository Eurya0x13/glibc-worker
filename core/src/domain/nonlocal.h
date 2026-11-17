#ifndef GW_DOMAIN_NONLOCAL_H
#define GW_DOMAIN_NONLOCAL_H
#include <setjmp.h>
#include <stdint.h>
#include <ucontext.h>
#include "nonlocal-mask.h"

/* Host-side native unwind boundaries. These do not describe resumable
 * ucontext continuations or permit bypassing Java frames. Interposition is
 * opt-in until the complete recovery contract has been validated. */
struct gw_nonlocal_checkpoint;

struct gw_nonlocal_scope {
  sigjmp_buf landing;
  struct gw_nonlocal_scope *volatile previous;
  struct gw_nonlocal_checkpoint *volatile captures;
  volatile uint64_t generation;
};

int gw_nonlocal_init(void);
int gw_nonlocal_ready(void);
void gw_nonlocal_enter(struct gw_nonlocal_scope *);
void gw_nonlocal_leave(struct gw_nonlocal_scope *);
int gw_nonlocal_pending(void);
/* Retire nested scopes and land in a still-live owning Host boundary. No
 * foreign libc buffer is interpreted or passed to the Host restore entry. */
_Noreturn void gw_nonlocal_route(struct gw_nonlocal_scope *);
void *gw_nonlocal_capture(void *buffer, unsigned kind, uintptr_t sp,
                          uintptr_t pc);
_Noreturn void gw_nonlocal_jump(void *buffer, int value, unsigned kind);
#ifndef __ANDROID__
int gw_nonlocal_setcontext(const ucontext_t *);
#endif
#endif
