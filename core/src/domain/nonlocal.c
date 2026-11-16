#define _GNU_SOURCE
#include "nonlocal.h"
#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

typedef void (*restore_fn)(void *, int);
static void *captures[3];
static restore_fn restores[4];
static _Thread_local struct gw_nonlocal_scope *top;
static _Thread_local uint64_t generation;
extern int gw_nonlocal_interposed(void) __attribute__((weak));
extern void signal_host_prune_notifications(void) __attribute__((weak));

static void prepare_native_restore(void) {
  /* Native mask restoration bypasses public sigprocmask. Retire invalidated
   * notification levels before it can unblock them at the target. */
  if (signal_host_prune_notifications) {
    signal_host_prune_notifications();
  }
}

int gw_nonlocal_ready(void) {
  return captures[0] && gw_nonlocal_interposed && gw_nonlocal_interposed();
}

struct checkpoint {
  void *buffer;
  struct gw_nonlocal_scope *scope;
  uint64_t generation;
};

/* Buffers remain opaque. Recording ownership before a tail call preserves
 * the libc's saved caller and its pointer/shadow-stack protection:
 * https://github.com/bminor/glibc/blob/glibc-2.41/sysdeps/x86_64/setjmp.S
 * https://android.googlesource.com/platform/bionic/+/main/libc/arch-arm64/bionic/setjmp.S
 * Pages are contiguous records and remain pinned in this integration stage.
 * Their reclamation must join the carrier/continuation lifetime protocol. */
struct checkpoint_page {
  struct checkpoint_page *next;
  struct checkpoint records[64];
};
static _Thread_local struct checkpoint_page *pages;

static _Thread_local struct {
  int active, routed;
  void *buffer;
  int value;
  restore_fn restore;
  struct gw_nonlocal_scope *target;
  uint64_t target_generation;
  uint64_t mask;
} pending;

int gw_nonlocal_init(void) {
  /* Bootstrap only, serialized before creating recipients. No resolution is
   * performed in a signal handler or while transferring a continuation. */
  if (captures[0]) {
    return 0;
  }
#ifdef __ANDROID__
  const char *save[] = {"sigsetjmp", "setjmp", "_setjmp"};
#else
  const char *save[] = {"__sigsetjmp", "setjmp", "_setjmp"};
#endif
  const char *jump[] = {"siglongjmp", "longjmp", "_longjmp"};
  void *resolved[6];
  for (unsigned i = 0; i < 3; ++i) {
    resolved[i] = dlsym(RTLD_NEXT, save[i]);
    resolved[i + 3] = dlsym(RTLD_NEXT, jump[i]);
    if (!resolved[i] || !resolved[i + 3]) {
      return ENOSYS;
    }
  }
#ifdef __GLIBC__
  /* Fortified callers redirect all longjmp spellings to this entry. Keep
   * libc's stack validity checks on the final restore rather than bypassing
   * them with the unchecked entry.
   * https://github.com/bminor/glibc/blob/glibc-2.41/setjmp/bits/setjmp2.h
   */
  restore_fn checked = (restore_fn)dlsym(RTLD_NEXT, "__longjmp_chk");
  if (!checked) {
    return ENOSYS;
  }
  restores[3] = checked;
#endif
  for (unsigned i = 0; i < 3; ++i) {
    restores[i] = (restore_fn)resolved[i + 3];
    captures[i] = resolved[i];
  }
  return 0;
}

static struct checkpoint *find_checkpoint(void *buffer, int create) {
  struct checkpoint *empty = NULL;
  for (struct checkpoint_page *p = pages; p; p = p->next) {
    for (unsigned i = 0; i < 64; ++i) {
      struct checkpoint *r = &p->records[i];
      if (r->buffer == buffer) {
        return r;
      }
      if (!r->buffer && !empty) {
        empty = r;
      }
    }
  }
  if (!create) {
    return NULL;
  }
  if (empty) {
    return empty;
  }
  /* Direct kernel allocation avoids interrupting/reentering libc's allocator.
   * syscall preserves the buffer's libc ABI; this code always has Host TLS. */
  struct checkpoint_page *p =
      (void *)syscall(SYS_mmap, NULL, sizeof *p, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) {
    _exit(125);
  }
  p->next = pages;
  pages = p;
  return &p->records[0];
}

void *gw_nonlocal_capture(void *buffer, unsigned kind) {
  if (kind >= 3 || !captures[kind]) {
    _exit(125);
  }
  int saved = errno;
  uint64_t mask = gw_nonlocal_mask_block();
  struct checkpoint *r = find_checkpoint(buffer, 1);
  *r = (struct checkpoint){buffer, top, top ? top->generation : 0};
  gw_nonlocal_mask_restore(mask);
  errno = saved;
  return captures[kind];
}

void gw_nonlocal_enter(struct gw_nonlocal_scope *scope) {
  uint64_t mask = gw_nonlocal_mask_block();
  if (++generation == 0) {
    _exit(125);
  }
  scope->generation = generation;
  scope->previous = top;
  top = scope;
  gw_nonlocal_mask_restore(mask);
}

int gw_nonlocal_pending(void) {
  return pending.active;
}

static _Noreturn void continue_transfer(void) {
  if (top == pending.target &&
      (!top || top->generation == pending.target_generation)) {
    void *buffer = pending.buffer;
    int value = pending.value;
    restore_fn restore = pending.restore;
    uint64_t mask = pending.mask;
    int routed = pending.routed;
    struct gw_nonlocal_scope *target = pending.target;
    pending.active = 0;
    pending.buffer = NULL;
    prepare_native_restore();
    if (routed) {
      /* The receiver cleans up with signals blocked, just like intermediate
       * landing scopes. Its caller owns restoration of the execution mask. */
      restores[0](target->landing, 1);
      __builtin_unreachable();
    }
    gw_nonlocal_mask_restore(mask);
    restore(buffer, value);
  } else if (top) {
    restores[0](top->landing, 1);
  }
  _exit(125);
}

void gw_nonlocal_leave(struct gw_nonlocal_scope *scope) {
  uint64_t mask = gw_nonlocal_mask_block();
  if (top != scope) {
    _exit(125);
  }
  top = scope->previous;
  if (pending.active) {
    continue_transfer();
  }
  gw_nonlocal_mask_restore(mask);
}

_Noreturn void gw_nonlocal_jump(void *buffer, int value, unsigned kind) {
  if (kind >= 4 || !restores[kind]) {
    _exit(125);
  }
  uint64_t mask = gw_nonlocal_mask_block();
  struct checkpoint *r = find_checkpoint(buffer, 0);
  if (!top || (r && r->scope == top && r->generation == top->generation)) {
    prepare_native_restore();
    gw_nonlocal_mask_restore(mask);
    restores[kind](buffer, value);
    __builtin_unreachable();
  }
  /* An unobserved capture cannot establish which live resources it skips.
   * This remains an integration failure, never a compatibility success. */
  if (!r || pending.active) {
    _exit(125);
  }
  struct gw_nonlocal_scope *ancestor = top;
  while (ancestor && ancestor != r->scope) {
    ancestor = ancestor->previous;
  }
  if (ancestor != r->scope ||
      (ancestor && ancestor->generation != r->generation)) {
    _exit(125);
  }
  pending.buffer = buffer;
  pending.active = 1;
  pending.routed = 0;
  pending.value = value;
  pending.restore = restores[kind];
  pending.target = r->scope;
  pending.target_generation = r->generation;
  pending.mask = mask;
  continue_transfer();
}

_Noreturn void gw_nonlocal_route(struct gw_nonlocal_scope *target) {
  uint64_t mask = gw_nonlocal_mask_block();
  struct gw_nonlocal_scope *ancestor = top;
  while (ancestor && ancestor != target) {
    ancestor = ancestor->previous;
  }
  if (!target || !ancestor || pending.active || !gw_nonlocal_ready()) {
    _exit(125);
  }
  pending.active = 1;
  pending.routed = 1;
  pending.target = target;
  pending.target_generation = target->generation;
  pending.mask = mask;
  continue_transfer();
}
