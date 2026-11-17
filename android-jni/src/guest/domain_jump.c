#define _GNU_SOURCE
#include "domain_jump.h"
#include "domain_transport.h"
#include "guest_internal.h"
#include "nonlocal-mask.h"
#include "signal-stack.h"
#include <errno.h>
#include <dlfcn.h>
#include <setjmp.h>
#include <sys/mman.h>

/* A buffer is indexed by address but belongs to an invocation generation.
 * Native buffers remain opaque. Retire records on invocation exit and reuse
 * slots; the carrier owns the high-water pages until its orderly teardown.
 * mmap avoids allocator reentry when a handler captures a continuation.
 * https://man7.org/linux/man-pages/man7/signal-safety.7.html
 */
struct jump_record {
  const void *buffer;
  struct gw_domain_native_checkpoint *owner;
  struct jni_domain_signal *signal;
  uint64_t generation, module;
  uintptr_t sp, pc;
  int context;
  int depth, rpc_top;
};

struct jump_page {
  struct jump_page *next;
  struct jump_record records[64];
};

enum { NATIVE_JUMP = 1, NATIVE_CHECKED_JUMP, NATIVE_SETCONTEXT };

struct context_copy {
  ucontext_t value;
#if defined(__x86_64__)
  struct _libc_fpstate fp;
#endif
};

static __thread struct jni_domain_context *carrier;
static __thread struct jump_page *pages;
static __thread uint64_t generation;
static __thread struct context_copy *pending_context;
static __thread const ucontext_t *restoring_context;
static void (*checked_jump)(void *, int);

int gw_domain_jump_init(void) {
  *(void **)&checked_jump = dlsym(RTLD_DEFAULT, "__longjmp_chk");
  return checked_jump ? 0 : ENOSYS;
}

static struct jump_record *lookup(const void *buffer, int create) {
  struct jump_record *empty = NULL;
  for (struct jump_page *p = pages; p; p = p->next) {
    for (unsigned i = 0; i < 64; ++i) {
      struct jump_record *r = &p->records[i];
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
  struct jump_page *p =
      (void *)syscall(SYS_mmap, NULL, sizeof *p, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) {
    _exit(125);
  }
  p->next = pages;
  pages = p;
  return &p->records[0];
}

void gw_domain_checkpoint_enter(struct gw_domain_native_checkpoint *frame) {
  if (!carrier) {
    return;
  }
  if (++generation == 0) {
    _exit(125);
  }
  frame->generation = generation;
  frame->previous = carrier->abort_checkpoint;
  carrier->abort_checkpoint = frame;
  carrier->abort_call = frame->call;
}

void gw_domain_checkpoint_leave(struct gw_domain_native_checkpoint *frame) {
  if (!carrier) {
    return;
  }
  if (carrier->abort_checkpoint != frame || carrier->jump_target == frame) {
    _exit(125);
  }
  for (struct jump_page *p = pages; p; p = p->next) {
    for (unsigned i = 0; i < 64; ++i) {
      if (p->records[i].owner == frame) {
        p->records[i] = (struct jump_record){0};
      }
    }
  }
  carrier->abort_checkpoint = frame->previous;
  carrier->abort_call = frame->previous ? frame->previous->call : NULL;
}

void gw_domain_jump_signal_leave(struct jni_domain_signal *signal) {
  for (struct jump_page *p = pages; p; p = p->next) {
    for (unsigned i = 0; i < 64; ++i) {
      if (p->records[i].signal == signal) {
        if (&p->records[i] == carrier->jump_record) {
          _exit(125);
        }
        p->records[i] = (struct jump_record){0};
      }
    }
  }
}

static void capture(struct jump_record *r, const void *buffer,
                    struct gw_domain_native_checkpoint *frame) {
  struct fb_context *context = fb_context_at(G, gid);
  *r = (struct jump_record){
      .buffer = buffer,
      .owner = frame,
      .signal = carrier->guest_signal,
      .generation = frame ? frame->generation : 0,
      .module = gmodule,
      .depth = gdepth,
      .rpc_top = atomic_load_explicit(&context->rpc_top, memory_order_relaxed)};
}

static int can_route(const struct jump_record *r,
                     struct gw_domain_native_checkpoint *frame) {
  struct gw_domain_native_checkpoint *ancestor = frame;
  while (ancestor && ancestor != r->owner) {
    ancestor = ancestor->previous;
  }
  if (ancestor != r->owner ||
      (ancestor && ancestor->generation != r->generation) ||
      carrier->jump_record) {
    return 0;
  }
  if (carrier->guest_signal == r->signal) {
    return ancestor != NULL;
  }
  if (!r->signal) {
    return ancestor && carrier->guest_signal && carrier->host_rpc &&
           carrier->guest_rpc_owner == ancestor;
  }

  struct jni_domain_signal *signal = carrier->guest_signal;
  while (signal && signal != r->signal) {
    signal = signal->previous;
  }
  return signal && r->owner == frame;
}

static _Noreturn void route(struct jump_record *r,
                            struct gw_domain_native_checkpoint *frame,
                            int value, int kind, uint64_t mask) {
  carrier->jump_record = r;
  carrier->jump_target = r->owner;
  carrier->jump_value = value;
  carrier->jump_kind = kind;
  extern uint64_t gw_domain_reserved_signals(void);
  uint64_t reserved = gw_domain_reserved_signals();
  carrier->guest_mask = (mask & ~reserved) | (carrier->guest_mask & reserved);
  if (carrier->guest_signal != r->signal) {
    /* Retire this activation before returning to an older signal or the
     * suspended native RPC. Host owns the intervening unwind scopes. */
    carrier->jump_signal_target = r->signal;
    gw_guest_signal_escape();
  }
  siglongjmp(*(sigjmp_buf *)frame->buffer, 1);
}

void gw_domain_jump_event(void *buffer, int value, int restore) {
  if (!carrier || gw_guest_signal_internal_jump(buffer)) {
    return;
  }
  struct gw_domain_native_checkpoint *frame = carrier->abort_checkpoint;
  if (!frame && !carrier->guest_signal) {
    return;
  }

  int saved = errno;
  uint64_t mask = gw_nonlocal_mask_block();
  struct jump_record *r = lookup(buffer, !restore);
  if (!restore) {
    capture(r, buffer, frame);
  } else if (!r || r->context) {
    _exit(125); /* Unknown ownership cannot skip live bridge resources. */
  } else if (r->owner != frame || r->signal != carrier->guest_signal) {
    if (!can_route(r, frame)) {
      _exit(125);
    }
    route(r, frame, value, restore, mask);
  }
  gw_nonlocal_mask_restore(mask);
  errno = saved;
}

static struct jump_record *lookup_context(const ucontext_t *context) {
  struct jump_record *r = lookup(context, 0);
  if (r && r->context) {
    return r;
  }

  /* A caller may pass a copy of an owned getcontext/swapcontext record. Match
   * its native continuation, and reject an ambiguous live scope. */
  uintptr_t pc = gw_signal_interrupted_pc(context);
  uintptr_t sp = gw_signal_interrupted_sp(context);
  struct jump_record *match = NULL;
  for (struct jump_page *p = pages; p; p = p->next) {
    for (unsigned i = 0; i < 64; ++i) {
      r = &p->records[i];
      if (!r->context || r->pc != pc || r->sp != sp) {
        continue;
      }
      if (match &&
          (match->generation != r->generation || match->signal != r->signal ||
           match->depth != r->depth || match->module != r->module ||
           match->rpc_top != r->rpc_top)) {
        return NULL;
      }
      match = r;
    }
  }
  return match;
}

static int retain_context(const ucontext_t *context) {
  struct context_copy *copy =
      (void *)syscall(SYS_mmap, NULL, sizeof *copy, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (copy == MAP_FAILED) {
    return errno;
  }
  copy->value = *context;
#if defined(__x86_64__)
  if (context->uc_mcontext.fpregs) {
    copy->fp = *context->uc_mcontext.fpregs;
  }
#endif
  pending_context = copy;
  return 0;
}

int gw_domain_context_event(ucontext_t *saved, const ucontext_t *target,
                            int resumable) {
  if (!carrier || (saved && saved == carrier->parked_guest) ||
      (target && target == carrier->parked_guest)) {
    return 0;
  }
  if (target && target == restoring_context) {
    restoring_context = NULL;
    return 0;
  }
  struct gw_domain_native_checkpoint *frame = carrier->abort_checkpoint;
  if (!frame && !carrier->guest_signal) {
    return 0;
  }

  int original_errno = errno;
  uint64_t mask = gw_nonlocal_mask_block();
  int error = 0;
  if (saved) {
    struct jump_record *r = lookup(saved, 1);
    capture(r, saved, frame);
    r->context = 1;
    r->pc = gw_signal_interrupted_pc(saved);
    r->sp = gw_signal_interrupted_sp(saved);
  } else {
    struct jump_record *r = lookup_context(target);
    if (!r) {
      error = ENOTSUP;
    } else if (r->owner != frame || r->signal != carrier->guest_signal) {
      if (resumable || !can_route(r, frame)) {
        error = ENOTSUP;
      } else if (!(error = retain_context(target))) {
        route(r, frame, 0, NATIVE_SETCONTEXT, mask);
      }
    }
  }
  gw_nonlocal_mask_restore(mask);
  errno = original_errno;
  return error;
}

static _Noreturn void resume_context(void) {
  struct context_copy restored = *pending_context;
  if (syscall(SYS_munmap, pending_context, sizeof restored)) {
    _exit(125);
  }
  pending_context = NULL;
#if defined(__x86_64__)
  if (restored.value.uc_mcontext.fpregs) {
    restored.value.uc_mcontext.fpregs = &restored.fp;
  }
#endif
  gw_domain_restore_context(&restored.value);
}

_Noreturn void gw_domain_restore_context(ucontext_t *target) {
  restoring_context = target;
  setcontext(target);
  _exit(125);
}

static _Noreturn void restore_guest(void *argument) {
  struct jni_domain_context *c = argument;
  struct jump_record *r = c->jump_record;
  struct gw_domain_native_checkpoint *frame = c->abort_checkpoint;
  if (!r || r->owner != frame || r->signal != c->guest_signal ||
      (frame && r->generation != frame->generation)) {
    _exit(125);
  }
  void *buffer = (void *)r->buffer;
  int value = c->jump_value;
  int kind = c->jump_kind;
  gdepth = r->depth;
  gmodule = r->module;
  atomic_store_explicit(&fb_context_at(G, gid)->rpc_top, r->rpc_top,
                        memory_order_release);
  c->jump_record = c->jump_target = NULL;
  c->jump_signal_target = NULL;
  c->guest_native_active = 1;
  if (!r->signal) {
    gw_signal_stack_set(&c->guest_altstack);
  }
  if (kind == NATIVE_SETCONTEXT) {
    resume_context();
  }
  extern uint64_t gw_domain_execution_mask(void);
  gw_nonlocal_mask_restore(gw_domain_execution_mask());
  if (kind == NATIVE_CHECKED_JUMP) {
    checked_jump(buffer, value);
    _exit(125);
  }
  siglongjmp(*(sigjmp_buf *)buffer, value);
}

int gw_domain_resume_signal(int unused, void *info, void *frame) {
  (void)unused;
  (void)info;
  (void)frame;
  if (!carrier || !carrier->guest_signal ||
      carrier->guest_signal != carrier->jump_signal_target) {
    _exit(125);
  }
  restore_guest(carrier);
}

void gw_domain_jump_bind(struct jni_domain_context *c) {
  carrier = c;
}

void gw_domain_rpc_finish(void) {
  if (carrier->abort_resume) {
    carrier->abort_resume = 0;
    struct gw_domain_native_checkpoint *frame = carrier->abort_checkpoint;
    siglongjmp(*(sigjmp_buf *)frame->buffer, 1);
  }
  if (carrier->jump_record &&
      carrier->jump_target == carrier->abort_checkpoint &&
      !carrier->jump_signal_target) {
    restore_guest(carrier);
  }
}

void gw_domain_jump_finish(void) {
  if (!carrier || carrier->abort_checkpoint || carrier->jump_record ||
      carrier->guest_signal || carrier->guest_rpc_owner ||
      carrier->jump_signal_target || carrier->abort_resume || pending_context ||
      restoring_context) {
    _exit(125);
  }
  while (pages) {
    struct jump_page *p = pages;
    pages = p->next;
    if (syscall(SYS_munmap, p, sizeof *p)) {
      _exit(125);
    }
  }
  carrier = NULL;
}
