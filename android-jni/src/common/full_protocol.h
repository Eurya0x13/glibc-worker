#ifndef FULL_PROTOCOL_H
#define FULL_PROTOCOL_H
#include <jni.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>
#include "stable_pages.h"
#include "wait_event.h"
#include "rpc_sequence.h"
#ifndef FB_CONTEXTS
#define FB_CONTEXTS 16
#endif
#define FB_DEPTH 12
/* An independent reserved slot reports frame-allocation failures and permits
   exception/cleanup JNI. It never starts another Guest native invocation. */
#define FB_RPC_REPORT_FAILURE (-1)

enum fb_failure {
  FB_FAILURE_MEMORY = 1,
  FB_FAILURE_DEPTH,
  FB_FAILURE_ARGUMENT,
  FB_FAILURE_STATE
};

_Static_assert(FB_DEPTH == GW_HANDOFF_DEPTH,
               "Initial frames cover the bounded diagnostic handoff window");
#define FB_ARGS 32
#define FB_SIG 256
#define FB_APIS 235
#include "module_id.h"

/* POD only, shared by two separately linked libffi/libc implementations. */
struct fb_rpc {
  int depth;
  _Atomic int request, response, active;
  uint64_t module;
  int api, error;
  jvalue a[8], result;
};

struct fb_call {
  _Atomic int request, response;
  uint64_t module;
  uintptr_t function;
  char sig[FB_SIG], diagnostic[512];
  jvalue args[FB_ARGS + 2], result;
  int op, error;
};

struct fb_frame {
  struct fb_call call;
  struct fb_rpc rpc;
};

struct fb_context {
  /* Never reset these epochs while recycling a context slot. */
  struct fb_event host_event, guest_event;
  _Atomic int state, closing, host_done, guest_done;
  /* Intrusive owner inbox node, independent of the reusable state fields.
     Only the producer holding owner_pending may change owner_next. */
  _Atomic int owner_pending, owner_next;
  _Atomic int origin;
  int host_tid, guest_tid, daemon, attach_result;
  char name[64];
  _Atomic(uintptr_t) host_private, guest_private;
  _Atomic(uintptr_t) host_metrics, guest_metrics;
  _Atomic int frame_capacity, rpc_top;
  struct fb_pages frames;
  struct fb_rpc emergency;
  struct fb_rpc rpc[FB_DEPTH];
  struct fb_call call[FB_DEPTH];
};

struct full_bridge {
  _Atomic int running, errors;
  struct fb_event host_control;
  struct fb_context ctx[FB_CONTEXTS];
  struct fb_pages contexts;
  _Atomic int context_capacity;
  _Atomic int owner_pending_head;
  _Atomic unsigned hits[FB_APIS];
  _Atomic int loads, unloads, constructors, destructors, live_guest_threads;
  _Atomic int native_calls, max_depth, live_global, live_weak, live_local,
      live_closures;
  _Atomic int active_modules, peak_modules, failed_loads,
      dependency_constructors, dependency_destructors, id_cache_weak;
  _Atomic int image_opens, image_closes;
  int suite_rc;
  char module_path[4096];
};

/* Inline capacities are initial reservations, not execution limits. Published
   pages and contexts never move or shrink, including across slot recycling. */
static inline int fb_context_capacity(struct full_bridge *b) {
  int count = atomic_load_explicit(&b->context_capacity, memory_order_acquire);
  return count > FB_CONTEXTS ? count : FB_CONTEXTS;
}

static inline struct fb_context *fb_context_at(struct full_bridge *b, int i) {
  if (!b || i < 0 || i >= fb_context_capacity(b)) {
    return NULL;
  }
  if (i < FB_CONTEXTS) {
    return &b->ctx[i];
  }

  return fb_pages_at(&b->contexts, (unsigned)(i - FB_CONTEXTS),
                     sizeof(struct fb_context), 0);
}

/* Many lifecycle producers, one Guest owner consumer. Stable context indices
   form a pending stack: no event allocation, capacity scan, or pointer into
   either libc's private storage. Index zero is reserved for the owner.
   Publish the state first. Even a coalesced post must be a release RMW so
   the consumer's acquire-clear observes that state update. */
static inline void fb_owner_post(struct full_bridge *b, int id) {
  struct fb_context *c = fb_context_at(b, id);
  if (atomic_exchange_explicit(&c->owner_pending, 1, memory_order_acq_rel)) {
    return;
  }
  int head = atomic_load_explicit(&b->owner_pending_head, memory_order_relaxed);
  do {
    atomic_store_explicit(&c->owner_next, head, memory_order_relaxed);
  } while (!atomic_compare_exchange_weak_explicit(&b->owner_pending_head, &head,
                                                  id, memory_order_release,
                                                  memory_order_relaxed));
  fb_event_notify(&b->ctx[0].guest_event);
}

/* Detach one finite batch before processing it, so a stream of new lifecycle
   changes cannot indefinitely postpone the owner's own command. Only gid=0
   may consume. A producer never dereferences the previous stack head, so
   index reuse cannot cause a stale-node pop (there is no producer-side pop). */
static inline int fb_owner_take(struct full_bridge *b) {
  if (!atomic_load_explicit(&b->owner_pending_head, memory_order_acquire)) {
    return 0;
  }
  return atomic_exchange_explicit(&b->owner_pending_head, 0,
                                  memory_order_acquire);
}

/* Save next BEFORE allowing this node to be posted again. Clear BEFORE
   inspecting its state: a concurrent state update is either acquired from
   the coalesced post, or enqueued for the next batch with its own wake. */
static inline int fb_owner_next(struct fb_context *c) {
  int next = atomic_load_explicit(&c->owner_next, memory_order_relaxed);
  atomic_exchange_explicit(&c->owner_pending, 0, memory_order_acq_rel);
  return next;
}

static inline int fb_frame_capacity(struct fb_context *c) {
  int count = atomic_load_explicit(&c->frame_capacity, memory_order_acquire);
  return count > FB_DEPTH ? count : FB_DEPTH;
}

static inline int fb_ensure_frame(struct fb_context *c, int depth) {
  if (depth < 0 || depth >= INT_MAX - FB_PAGE_ITEMS - FB_DEPTH) {
    return 0;
  }

  if (depth < FB_DEPTH) {
    return 1;
  }

  if (!fb_pages_at(&c->frames, (unsigned)(depth - FB_DEPTH),
                   sizeof(struct fb_frame), 1)) {
    return 0;
  }
  int count =
      FB_DEPTH + ((depth - FB_DEPTH) / FB_PAGE_ITEMS + 1) * FB_PAGE_ITEMS;
  int old = atomic_load_explicit(&c->frame_capacity, memory_order_relaxed);
  while (old < count && !atomic_compare_exchange_weak_explicit(
                            &c->frame_capacity, &old, count,
                            memory_order_release, memory_order_relaxed)) {
  }
  return 1;
}

static inline struct fb_frame *fb_frame_at(struct fb_context *c, int depth) {
  return fb_pages_at(&c->frames, (unsigned)(depth - FB_DEPTH),
                     sizeof(struct fb_frame), 0);
}

static inline struct fb_call *fb_call_at(struct fb_context *c, int depth) {
  if (!c || depth < 0 || depth >= fb_frame_capacity(c)) {
    return NULL;
  }
  if (depth < FB_DEPTH) {
    return &c->call[depth];
  }

  struct fb_frame *frame = fb_frame_at(c, depth);
  return frame ? &frame->call : NULL;
}

static inline struct fb_rpc *fb_rpc_at(struct fb_context *c, int depth) {
  if (!c || depth < 0 || depth >= fb_frame_capacity(c)) {
    return NULL;
  }
  if (depth < FB_DEPTH) {
    return &c->rpc[depth];
  }

  struct fb_frame *frame = fb_frame_at(c, depth);
  return frame ? &frame->rpc : NULL;
}

static inline void fb_context_reset(struct fb_context *c) {
  atomic_store(&c->closing, 0);
  atomic_store(&c->host_done, 0);
  atomic_store(&c->guest_done, 0);
  atomic_store(&c->origin, 0);
  atomic_store(&c->rpc_top, -1);
  c->host_tid = c->guest_tid = c->daemon = c->attach_result = 0;
  memset(c->name, 0, sizeof c->name);
}

enum {
  FB_FREE = 0,
  FB_HOST_REQUEST = 1,
  FB_RUNNING = 2,
  FB_CLOSING = 3,
  FB_DEAD = 4,
  FB_JOINED = 5,
  FB_GUEST_ATTACH = 6,
  FB_ATTACH_STARTING = 7,
  FB_RESERVED = 8,
  FB_ATTACH_FAILED = 9
};

/* Host and Guest may reserve concurrently. CAS owns an individual slot;
   radix publication owns growth. Losing allocations are unmapped locally. */
static inline int fb_reserve_context(struct full_bridge *b) {
  for (;;) {
    int count = fb_context_capacity(b);
    for (int i = 1; i < count; i++) {
      struct fb_context *c = fb_context_at(b, i);
      int state = FB_FREE;
      if (atomic_compare_exchange_strong(&c->state, &state, FB_RESERVED)) {
        return i;
      }
    }

    if (count > INT_MAX - FB_PAGE_ITEMS) {
      return -1;
    }
    if (!fb_pages_at(&b->contexts, (unsigned)(count - FB_CONTEXTS),
                     sizeof(struct fb_context), 1)) {
      return -1;
    }

    int next = count + FB_PAGE_ITEMS;
    int old = atomic_load_explicit(&b->context_capacity, memory_order_relaxed);
    while (old < next && !atomic_compare_exchange_weak_explicit(
                             &b->context_capacity, &old, next,
                             memory_order_release, memory_order_relaxed)) {
    }
  }
}

enum {
  FB_NATIVE = 1,
  FB_LOAD,
  FB_UNLOAD,
  FB_LOOKUP,
  FB_ABORT_LOAD,
  FB_COMMIT_LOAD,
  FB_OWNER,
  FB_PREPARE_UNLOAD
};

/* Diagnostic bootstrap only: call[0] is idle until loader activation. These
 * markers use existing POD slots and never change the shared structure ABI. */
#define FB_RPC_PROFILE_HELLO 0x47575250
#define FB_RPC_PROFILE_ACK 0x47575241
/* Optional event encoding requires an active Guest acknowledgement before
 * loader activation, even in a non-diagnostic build. Slots 7/8 are otherwise
 * unused bootstrap POD arguments; the old profile hello occupies slots 0..6. */
#define FB_WAKE_FILTER_HELLO 0x47574648
#define FB_WAKE_FILTER_ACK 0x47574641
/* JNI capabilities use the idle bootstrap call's slots 10..12. This handshake
 * is mandatory even when diagnostic profiling and wake filtering are off. */
#define FB_JNI_HELLO 0x47574a48
#define FB_JNI_ACK 0x47574a41
/* Mandatory private loader protocol: transactional unload, layout and module
   capacity. Use idle bootstrap args, preserving shared POD layout. Publish the
   ACK with the existing release/acquire phase-ready edge before any module can
   open. */
#define FB_LOADER_PROTOCOL 6
#define FB_LOADER_HELLO 0x47574c48
#define FB_LOADER_ACK 0x47574c41

static inline void fb_loader_offer(struct fb_call *hello) {
  hello->args[13].j = FB_LOADER_HELLO;
  hello->args[14].j = FB_LOADER_PROTOCOL;
  hello->args[15].j = 0;
  hello->args[16].j = sizeof(struct full_bridge);
  hello->args[17].j = FB_CONTEXTS;
  hello->args[18].j = FB_MODULES;
  hello->args[19].j = sizeof(struct fb_context);
  hello->args[20].j = sizeof(struct fb_rpc);
  hello->args[21].j = sizeof(struct fb_call);
  hello->args[22].j = FB_DEPTH;
  hello->args[23].j = FB_PAGE_ITEMS;
}

static inline int fb_loader_matches(const struct fb_call *hello) {
  return hello->args[13].j == FB_LOADER_HELLO &&
         hello->args[14].j == FB_LOADER_PROTOCOL &&
         hello->args[16].j == sizeof(struct full_bridge) &&
         hello->args[17].j == FB_CONTEXTS && hello->args[18].j == FB_MODULES &&
         hello->args[19].j == sizeof(struct fb_context) &&
         hello->args[20].j == sizeof(struct fb_rpc) &&
         hello->args[21].j == sizeof(struct fb_call) &&
         hello->args[22].j == FB_DEPTH && hello->args[23].j == FB_PAGE_ITEMS;
}

static inline int fb_loader_acknowledge(struct fb_call *hello) {
  hello->args[15].j = 0;
  if (!fb_loader_matches(hello)) {
    return 0;
  }
  hello->args[15].j = FB_LOADER_ACK;
  return 1;
}

static inline int fb_loader_accepted(const struct fb_call *hello) {
  return fb_loader_matches(hello) && hello->args[15].j == FB_LOADER_ACK;
}
#endif
