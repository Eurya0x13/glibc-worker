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
  void *buffer;
  struct gw_domain_native_checkpoint *owner;
  struct jni_domain_signal *signal;
  uint64_t generation, module;
  int depth, rpc_top;
};

struct jump_page {
  struct jump_page *next;
  struct jump_record records[64];
};

static __thread struct jni_domain_context *carrier;
static __thread struct jump_page *pages;
static __thread uint64_t generation;
static void (*checked_jump)(void *, int);

int gw_domain_jump_init(void) {
  *(void **)&checked_jump = dlsym(RTLD_DEFAULT, "__longjmp_chk");
  return checked_jump ? 0 : ENOSYS;
}

static struct jump_record *lookup(void *buffer, int create) {
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

static void capture(struct jump_record *r, void *buffer,
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

static _Noreturn void route(struct jump_record *r,
                            struct gw_domain_native_checkpoint *frame,
                            int value, int kind, uint64_t mask) {
  struct gw_domain_native_checkpoint *ancestor = frame;
  while (ancestor && ancestor != r->owner) {
    ancestor = ancestor->previous;
  }
  if (!ancestor || ancestor->generation != r->generation ||
      carrier->jump_record) {
    _exit(125);
  }

  carrier->jump_record = r;
  carrier->jump_target = ancestor;
  carrier->jump_value = value;
  carrier->jump_kind = kind;
  extern uint64_t gw_domain_reserved_signals(void);
  uint64_t reserved = gw_domain_reserved_signals();
  carrier->guest_mask = (mask & ~reserved) | (carrier->guest_mask & reserved);
  if (carrier->guest_signal != r->signal) {
    /* The signal arrived through an owning GNU entry while this native RPC
     * was suspended. Its independent activation must retire before the
     * Host C callback and the original Guest RPC can finish. */
    if (!carrier->guest_signal || r->signal ||
        carrier->guest_rpc_owner != ancestor) {
      _exit(125);
    }
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
  } else if (!r) {
    _exit(125); /* Unknown ownership cannot skip live bridge resources. */
  } else if (r->owner != frame || r->signal != carrier->guest_signal) {
    route(r, frame, value, restore, mask);
  }
  gw_nonlocal_mask_restore(mask);
  errno = saved;
}

static void restore_guest(void *argument) {
  struct jni_domain_context *c = argument;
  struct jump_record *r = c->jump_record;
  struct gw_domain_native_checkpoint *frame = c->abort_checkpoint;
  if (!r || r->owner != frame || r->generation != frame->generation) {
    _exit(125);
  }
  void *buffer = r->buffer;
  int value = c->jump_value;
  int checked = c->jump_kind == 2;
  gdepth = r->depth;
  gmodule = r->module;
  atomic_store_explicit(&fb_context_at(G, gid)->rpc_top, r->rpc_top,
                        memory_order_release);
  c->jump_record = c->jump_target = NULL;
  c->guest_native_active = 1;
  gw_signal_stack_set(&c->guest_altstack);
  extern uint64_t gw_domain_execution_mask(void);
  gw_nonlocal_mask_restore(gw_domain_execution_mask());
  if (checked) {
    checked_jump(buffer, value);
    _exit(125);
  }
  siglongjmp(*(sigjmp_buf *)buffer, value);
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
      carrier->jump_target == carrier->abort_checkpoint) {
    restore_guest(carrier);
  }
}

void gw_domain_jump_finish(void) {
  if (!carrier || carrier->abort_checkpoint || carrier->jump_record ||
      carrier->guest_signal || carrier->guest_rpc_owner ||
      carrier->abort_resume) {
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
