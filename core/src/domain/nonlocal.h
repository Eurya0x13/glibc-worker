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

enum gw_nonlocal_transfer_kind {
  GW_NONLOCAL_IDLE,
  GW_NONLOCAL_RESTORE,
  GW_NONLOCAL_ROUTE,
  GW_NONLOCAL_RESUME
};

struct gw_nonlocal_transfer {
  enum gw_nonlocal_transfer_kind kind;
  void *buffer;
  int value;
  void (*restore)(void *, int);
  void (*resume)(void *);
  struct gw_nonlocal_scope *target;
  uint64_t target_generation, mask;
  uintptr_t jump_sp;
};

int gw_nonlocal_init(void);
int gw_nonlocal_ready(void);
int gw_nonlocal_finish_thread(void);
void gw_nonlocal_enter(struct gw_nonlocal_scope *);
void gw_nonlocal_leave(struct gw_nonlocal_scope *);
int gw_nonlocal_pending(void);
const void *gw_nonlocal_scope_current(void);
/* Select between live Host scope chains on this carrier. Delivery is masked
 * and both chains stay pinned throughout this operation. */
int gw_nonlocal_select(struct gw_nonlocal_scope *, struct gw_nonlocal_scope *);
/* Retire nested scopes and land in a still-live owning Host boundary. No
 * foreign libc buffer is interpreted or passed to the Host restore entry. */
_Noreturn void gw_nonlocal_route(struct gw_nonlocal_scope *);
/* Borrow the transfer state while retiring a separately parked suffix. The
 * saved state remains on the surviving caller's stack throughout retirement. */
void gw_nonlocal_suspend_transfer(struct gw_nonlocal_transfer *);
void gw_nonlocal_restore_transfer(const struct gw_nonlocal_transfer *);
_Noreturn void gw_nonlocal_retire_to(struct gw_nonlocal_scope *,
                                     void (*)(void *), void *);
void *gw_nonlocal_capture(void *buffer, unsigned kind, uintptr_t sp,
                          uintptr_t pc);
_Noreturn void gw_nonlocal_jump(void *buffer, int value, unsigned kind);
#ifndef __ANDROID__
int gw_nonlocal_setcontext(const ucontext_t *);
#endif
#endif
