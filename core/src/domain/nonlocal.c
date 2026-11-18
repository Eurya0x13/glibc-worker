#define _GNU_SOURCE
#include "nonlocal.h"
#include "signal-stack.h"
#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

typedef void (*restore_fn)(void *, int);

enum { CONTEXT_CAPTURE = 3, SWAP_CAPTURE, CAPTURE_COUNT };

static void *captures[CAPTURE_COUNT];
static restore_fn restores[4];
static _Thread_local struct gw_nonlocal_scope *top;
static _Thread_local uint64_t generation;
extern int gw_nonlocal_interposed(void) __attribute__((weak));
extern void signal_host_prune_notifications(void) __attribute__((weak));
extern void gw_guest_fd_lease_end(const void *) __attribute__((weak));
extern void gw_guest_fd_lease_unwind(uintptr_t, uintptr_t)
    __attribute__((weak));
extern void gw_guest_fd_lease_context(uintptr_t, uintptr_t, size_t)
    __attribute__((weak));
extern int signal_host_stack_bounds(uintptr_t *, size_t *)
    __attribute__((weak));
#ifndef __ANDROID__
static int (*native_setcontext)(const ucontext_t *);
extern uint64_t signal_host_logical_mask(uint64_t) __attribute__((weak));
extern uint64_t signal_host_physical_mask(uint64_t) __attribute__((weak));
#endif

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

struct gw_nonlocal_checkpoint {
  const void *buffer;
  struct gw_nonlocal_scope *scope;
  struct gw_nonlocal_checkpoint *next;
  /* The scope and its live records stay pinned while delivery is masked. */
  struct gw_nonlocal_checkpoint *volatile *owner_link;
  uint64_t generation;
  uintptr_t sp, pc;
  unsigned kind;
};

/* Buffers remain opaque. Recording ownership before a tail call preserves
 * the libc's saved caller and its pointer/shadow-stack protection:
 * https://github.com/bminor/glibc/blob/glibc-2.41/sysdeps/x86_64/setjmp.S
 * https://android.googlesource.com/platform/bionic/+/main/libc/arch-arm64/bionic/setjmp.S
 * Slots from retired scopes are reused. The owning thread retains its
 * high-water pages and captures made outside bridge scopes. */
struct checkpoint_page {
  struct checkpoint_page *next;
  struct gw_nonlocal_checkpoint records[64];
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
  uintptr_t jump_sp;
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
#ifndef __ANDROID__
  native_setcontext = dlsym(RTLD_NEXT, "setcontext");
  captures[CONTEXT_CAPTURE] = dlsym(RTLD_NEXT, "getcontext");
  captures[SWAP_CAPTURE] = dlsym(RTLD_NEXT, "swapcontext");
  if (!native_setcontext || !captures[CONTEXT_CAPTURE] ||
      !captures[SWAP_CAPTURE]) {
    return ENOSYS;
  }
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

static struct gw_nonlocal_checkpoint *find_checkpoint(const void *buffer,
                                                      int create) {
  struct gw_nonlocal_checkpoint *empty = NULL;
  for (struct checkpoint_page *p = pages; p; p = p->next) {
    for (unsigned i = 0; i < 64; ++i) {
      struct gw_nonlocal_checkpoint *r = &p->records[i];
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

static void unlink_checkpoint(struct gw_nonlocal_checkpoint *record) {
  if (record->owner_link) {
    *record->owner_link = record->next;
    if (record->next) {
      record->next->owner_link = record->owner_link;
    }
  }
}

static void record_capture(void *buffer, unsigned kind, uintptr_t sp,
                           uintptr_t pc) {
  if (!top || buffer != (void *)top->landing || kind != 0) {
    struct gw_nonlocal_checkpoint *r = find_checkpoint(buffer, 1);
    unlink_checkpoint(r);
    *r =
        (struct gw_nonlocal_checkpoint){.buffer = buffer,
                                        .scope = top,
                                        .generation = top ? top->generation : 0,
                                        .kind = kind,
                                        .sp = sp,
                                        .pc = pc};
    if (kind >= CONTEXT_CAPTURE) {
      r->kind = CONTEXT_CAPTURE;
    }
    if (top) {
      r->next = top->captures;
      if (r->next) {
        r->next->owner_link = &r->next;
      }
      r->owner_link = &top->captures;
      top->captures = r;
    }
  }
}

void *gw_nonlocal_capture(void *buffer, unsigned kind, uintptr_t sp,
                          uintptr_t pc) {
  if (kind >= CAPTURE_COUNT || !captures[kind]) {
    _exit(125);
  }
  int saved = errno;
  uint64_t mask = gw_nonlocal_mask_block();
  record_capture(buffer, kind, sp, pc);
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
  scope->captures = NULL;
  top = scope;
  gw_nonlocal_mask_restore(mask);
}

int gw_nonlocal_pending(void) {
  return pending.active;
}

const void *gw_nonlocal_scope_current(void) {
  return top;
}

int gw_nonlocal_select(struct gw_nonlocal_scope *from,
                       struct gw_nonlocal_scope *to) {
  if (top != from || !to || !to->generation || pending.active) {
    return EBUSY;
  }
  top = to;
  return 0;
}

static void retire_jumped_descriptors(uintptr_t sp) {
  if (sp && gw_guest_fd_lease_unwind) {
    uintptr_t low;
    size_t size;
    uintptr_t top =
        signal_host_stack_bounds && signal_host_stack_bounds(&low, &size)
            ? low + size
            : 0;
    gw_guest_fd_lease_unwind(sp, top);
  }
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
    retire_jumped_descriptors(pending.jump_sp);
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
  if (gw_guest_fd_lease_end) {
    gw_guest_fd_lease_end(scope);
  }
  while (scope->captures) {
    struct gw_nonlocal_checkpoint *r = scope->captures;
    unlink_checkpoint(r);
    *r = (struct gw_nonlocal_checkpoint){0};
  }
  top = scope->previous;
  if (pending.active) {
    continue_transfer();
  }
  gw_nonlocal_mask_restore(mask);
}

static int live_checkpoint(const struct gw_nonlocal_checkpoint *record) {
  struct gw_nonlocal_scope *ancestor = top;
  while (ancestor && ancestor != record->scope) {
    ancestor = ancestor->previous;
  }
  return ancestor == record->scope &&
         (!ancestor || ancestor->generation == record->generation);
}

static _Noreturn void transfer(const struct gw_nonlocal_checkpoint *record,
                               void *buffer, int value, restore_fn restore,
                               uint64_t mask) {
  pending.buffer = buffer;
  pending.active = 1;
  pending.routed = 0;
  pending.value = value;
  pending.restore = restore;
  pending.target = record->scope;
  pending.target_generation = record->generation;
  pending.mask = mask;
  pending.jump_sp = record->kind == CONTEXT_CAPTURE ? 0 : record->sp;
  continue_transfer();
}

_Noreturn void gw_nonlocal_jump(void *buffer, int value, unsigned kind) {
  if (kind >= 4 || !restores[kind]) {
    _exit(125);
  }
  uint64_t mask = gw_nonlocal_mask_block();
  struct gw_nonlocal_checkpoint *r = find_checkpoint(buffer, 0);
  if (!top || (r && r->kind != CONTEXT_CAPTURE && r->scope == top &&
               r->generation == top->generation)) {
    retire_jumped_descriptors(r ? r->sp : 0);
    prepare_native_restore();
    gw_nonlocal_mask_restore(mask);
    restores[kind](buffer, value);
    __builtin_unreachable();
  }
  /* An unobserved capture cannot establish which live resources it skips.
   * This remains an integration failure, never a compatibility success. */
  if (!r || r->kind == CONTEXT_CAPTURE || pending.active ||
      !live_checkpoint(r)) {
    _exit(125);
  }
  transfer(r, buffer, value, restores[kind], mask);
}

#ifndef __ANDROID__
struct host_context_copy {
  ucontext_t value;
#if defined(__x86_64__)
  struct _libc_fpstate fp;
#endif
};

static struct gw_nonlocal_checkpoint *find_context(const ucontext_t *context) {
  struct gw_nonlocal_checkpoint *record = find_checkpoint(context, 0);
  if (record && record->kind == CONTEXT_CAPTURE) {
    return record;
  }

  uintptr_t sp = gw_signal_interrupted_sp(context);
  uintptr_t pc = gw_signal_interrupted_pc(context);
  struct gw_nonlocal_checkpoint *match = NULL;
  for (struct checkpoint_page *p = pages; p; p = p->next) {
    for (unsigned i = 0; i < 64; ++i) {
      record = &p->records[i];
      if (record->kind != CONTEXT_CAPTURE || record->sp != sp ||
          record->pc != pc || !live_checkpoint(record)) {
        continue;
      }
      if (match && (match->scope != record->scope ||
                    match->generation != record->generation)) {
        return NULL;
      }
      match = record;
    }
  }
  return match;
}

static int restore_context(ucontext_t *context, uint64_t mask) {
  /* Saved makecontext stacks can still be resumed. Limit lease retirement to
   * a known stack containing the destination and the waiter. */
  uintptr_t low, sp = gw_signal_interrupted_sp(context);
  size_t size;
  if (gw_guest_fd_lease_context && signal_host_stack_bounds &&
      signal_host_stack_bounds(&low, &size) && sp - low < size) {
    gw_guest_fd_lease_context(sp, low, size);
  }
  prepare_native_restore();
  if (signal_host_logical_mask && signal_host_physical_mask) {
    uint64_t saved;
    memcpy(&saved, &context->uc_sigmask, sizeof saved);
    saved = signal_host_physical_mask(signal_host_logical_mask(saved));
    memcpy(&context->uc_sigmask, &saved, sizeof saved);
  }
  int result = native_setcontext(context);
  int error = errno;
  gw_nonlocal_mask_restore(mask);
  errno = error;
  return result;
}

static void finish_context_transfer(void *buffer, int unused) {
  (void)unused;
  struct host_context_copy restored = *(struct host_context_copy *)buffer;
  if (syscall(SYS_munmap, buffer, sizeof restored)) {
    _exit(125);
  }
#if defined(__x86_64__)
  if (restored.value.uc_mcontext.fpregs) {
    restored.value.uc_mcontext.fpregs = &restored.fp;
  }
#endif
  restore_context(&restored.value, UINT64_MAX);
  _exit(125);
}

int gw_nonlocal_setcontext(const ucontext_t *context) {
  if (!native_setcontext) {
    errno = ENOSYS;
    return -1;
  }
  uint64_t mask = gw_nonlocal_mask_block();
  struct gw_nonlocal_checkpoint *record = top ? find_context(context) : NULL;
  if (!top || (record && record->scope == top && live_checkpoint(record))) {
    ucontext_t direct = *context;
    return restore_context(&direct, mask);
  }
  if (!record || pending.active || !live_checkpoint(record)) {
    gw_nonlocal_mask_restore(mask);
    errno = ENOTSUP;
    return -1;
  }

  struct host_context_copy *copy =
      (void *)syscall(SYS_mmap, NULL, sizeof *copy, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (copy == MAP_FAILED) {
    int error = errno;
    gw_nonlocal_mask_restore(mask);
    errno = error;
    return -1;
  }
  copy->value = *context;
#if defined(__x86_64__)
  if (context->uc_mcontext.fpregs) {
    copy->fp = *context->uc_mcontext.fpregs;
  }
#endif
  transfer(record, copy, 0, finish_context_transfer, UINT64_MAX);
}

#endif

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
