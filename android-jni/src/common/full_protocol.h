#ifndef FULL_PROTOCOL_H
#define FULL_PROTOCOL_H
#include <jni.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>
#include "wait_event.h"
#include "rpc_sequence.h"
#ifndef FB_CONTEXTS
#define FB_CONTEXTS 16
#endif
#define FB_DEPTH 12
_Static_assert(FB_DEPTH == GW_HANDOFF_DEPTH,
               "Diagnostic depth slots must follow the protocol");
#define FB_ARGS 32
#define FB_SIG 256
#define FB_APIS 235
#include "module_id.h"

/* POD only, shared by two separately linked libffi/libc implementations. */
struct fb_rpc {
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

struct fb_context {
  /* Never reset these epochs while recycling a context slot. */
  struct fb_event host_event, guest_event;
  _Atomic int state, closing, host_done, guest_done;
  _Atomic int origin;
  int host_tid, guest_tid, daemon;
  char name[64];
  struct fb_rpc rpc[FB_DEPTH];
  struct fb_call call[FB_DEPTH];
};

struct full_bridge {
  _Atomic int running, errors;
  struct fb_event host_control;
  struct fb_context ctx[FB_CONTEXTS];
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
/* Mandatory private loader protocol: v2 splits OnUnload and final dlclose.
   Use idle bootstrap args, preserving shared POD layout. Publish the ACK with
   the existing release/acquire phase-ready edge before any module can open. */
#define FB_LOADER_PROTOCOL 2
#define FB_LOADER_HELLO 0x47574c48
#define FB_LOADER_ACK 0x47574c41

static inline void fb_loader_offer(struct fb_call *hello) {
  hello->args[13].j = FB_LOADER_HELLO;
  hello->args[14].j = FB_LOADER_PROTOCOL;
  hello->args[15].j = 0;
  hello->args[16].j = sizeof(struct full_bridge);
  hello->args[17].j = FB_CONTEXTS;
}

static inline int fb_loader_matches(const struct fb_call *hello) {
  return hello->args[13].j == FB_LOADER_HELLO &&
         hello->args[14].j == FB_LOADER_PROTOCOL &&
         hello->args[16].j == sizeof(struct full_bridge) &&
         hello->args[17].j == FB_CONTEXTS;
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
