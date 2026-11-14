#define _GNU_SOURCE
#include <jni.h>
#include <ffi.h>
#include <dlfcn.h>
#include <link.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "protocol.h"
#include "full_bridge.h"
#include "guest_internal.h"
#include "jni_capabilities.h"

struct full_bridge *G;
__thread int gid = -1, gdepth;
JavaVM fg_vm;
static jint guest_jni_version;

struct fg_extended_table {
  struct JNINativeInterface base;
  jobject (*GetModule)(JNIEnv *, jclass);
};

static const struct fg_extended_table fg_table;
__thread JNIEnv fg_env = &fg_table.base;

struct guest_slot {
  pthread_t worker;
  int worker_live;
#if GW_RPC_PROFILE
  struct gw_rpc_row metrics;
#endif
};

static struct guest_slot *guest_slot(int id, int create) {
  struct fb_context *c = fb_context_at(G, id);
  if (!c) {
    return NULL;
  }
  uintptr_t value =
      atomic_load_explicit(&c->guest_private, memory_order_acquire);
  if (!value && create) {
    struct guest_slot *fresh = calloc(1, sizeof *fresh);
    if (!fresh) {
      return NULL;
    }
    if (atomic_compare_exchange_strong_explicit(
            &c->guest_private, &value, (uintptr_t)fresh, memory_order_acq_rel,
            memory_order_acquire)) {
      value = (uintptr_t)fresh;
    } else {
      free(fresh);
    }
  }
  struct guest_slot *slot = (void *)value;
#if GW_RPC_PROFILE
  if (slot) {
    atomic_store_explicit(&c->guest_metrics, (uintptr_t)&slot->metrics,
                          memory_order_release);
  }
#endif
  return slot;
}

__thread uint64_t gmodule;
#if GW_RPC_PROFILE
_Thread_local struct gw_rpc_row *gw_rpc_current
    __attribute__((visibility("hidden")));
_Thread_local struct gw_rpc_scope *gw_rpc_scope_current
    __attribute__((visibility("hidden")));
static struct gw_rpc_stage_cost guest_clock_floor;

struct gw_handoff_row *gw_rpc_peer_handoff(unsigned context) {
  struct fb_context *c = fb_context_at(G, (int)context);
  struct gw_rpc_row *row =
      c ? (void *)atomic_load_explicit(&c->host_metrics, memory_order_acquire)
        : NULL;
  return row ? &row->handoff : NULL;
}

static void metrics_connect(struct shared *shared) {
  struct fb_call *hello = &fb_context_at(G, 0)->call[0];

  if (hello->op != FB_RPC_PROFILE_HELLO ||
      hello->args[0].j != GW_RPC_METRICS_ABI ||
      hello->args[1].j != (jlong)sizeof *G || hello->args[2].j != FB_CONTEXTS ||
      hello->args[3].j != GW_RPC_SPIN_NS ||
      hello->args[4].j != (jlong)sizeof(struct gw_rpc_row) ||
      hello->args[5].j != GW_RPC_BREAKDOWN_ABI || !hello->args[9].j) {
    dprintf(2,
            "RPC_PROFILE_MISMATCH Guest requires matching Host/context/spin "
            "layout\n");
    atomic_store_explicit(&shared->phase, -1, memory_order_release);
    syscall(SYS_exit, 93);
    __builtin_unreachable();
  }

  gw_rpc_calibrate(&guest_clock_floor);

  hello->args[6].j = (jlong)(uintptr_t)&guest_clock_floor;
  struct guest_slot *owner = guest_slot(0, 1);
  if (!owner) {
    atomic_store_explicit(&shared->phase, -1, memory_order_release);
    syscall(SYS_exit, 93);
    __builtin_unreachable();
  }
  gw_rpc_current = &owner->metrics;
  hello->result.j = (jlong)(uintptr_t)gw_rpc_current;
  hello->error = FB_RPC_PROFILE_ACK;
  /* The owner's existing release/acquire phase-ready handshake publishes
   * this pointer. No request epoch is advanced; this is not an RPC. */
}
#endif
#include "generated/guest-jni.inc"

static int version_ok(jint v) {
  return gw_jni_accepts(guest_jni_version, v);
}

static jint fg_getenv(JavaVM *vm, void **env, jint version) {
  (void)vm;
  if (G) {
    atomic_fetch_add(&G->hits[233], 1);
  }

  if (!env) {
    return JNI_ERR;
  }
  *env = NULL;
  if (!version_ok(version)) {
    return JNI_EVERSION;
  }
  if (gid < 0) {
    return JNI_EDETACHED;
  }

  *env = &fg_env;
  return JNI_OK;
}

static jint fg_attach_common(JavaVM *vm, JNIEnv **env, void *opaque,
                             int daemon) {
  (void)vm;
  if (G) {
    atomic_fetch_add(&G->hits[daemon ? 234 : 231], 1);
  }
  if (!env) {
    return JNI_ERR;
  }
  *env = NULL;
  if (!G) {
    return JNI_ERR;
  }
  JavaVMAttachArgs *args = opaque;
  if (args && !version_ok(args->version)) {
    return JNI_EVERSION;
  }

  if (gid >= 0) {
    *env = &fg_env;
    return JNI_OK;
  }
  if (args && args->group) {
    return JNI_ERR; /* Explicitly unsupported non-null ThreadGroup. */
  }

  int i = fb_reserve_context(G);
  if (i >= 0) {
    struct fb_context *c = fb_context_at(G, i);
    struct guest_slot *slot = guest_slot(i, 1);
    if (!slot) {
      atomic_store_explicit(&c->state, FB_FREE, memory_order_release);
      return JNI_ENOMEM;
    }
#if GW_RPC_PROFILE
    gw_rpc_current = &slot->metrics;
#endif
    fb_context_reset(c);
    c->origin = 1;
    c->daemon = daemon;
    c->guest_tid = (int)syscall(SYS_gettid);
    if (args && args->name) {
      snprintf(c->name, sizeof c->name, "%s", args->name);
    }

    atomic_store_explicit(&c->state, FB_GUEST_ATTACH, memory_order_release);
    fb_event_notify(&G->host_control);
    for (;;) {
      uint32_t observed = fb_event_sample(&c->guest_event);
      int attached = atomic_load_explicit(&c->state, memory_order_acquire);
      if (attached == FB_RUNNING) {
        break;
      }
      if (attached == FB_ATTACH_FAILED) {
        jint result = c->attach_result != JNI_OK ? c->attach_result : JNI_ERR;
#if GW_RPC_PROFILE
        /* Retire the writer before the controller can recycle this
         * slot. The final control wake has no context metrics row. */
        gw_rpc_current = NULL;
#endif
        atomic_store_explicit(&c->guest_done, 1, memory_order_release);
        fb_event_notify(&G->host_control);
        return result; /* guest_done permits immediate slot reuse. */
      }
      fb_event_wait(&c->guest_event, observed);
    }

    gid = i;
    gdepth = 0;
    *env = &fg_env;
    return JNI_OK;
  }
  return JNI_ENOMEM;
}

static jint fg_attach(JavaVM *vm, JNIEnv **env, void *args) {
  return fg_attach_common(vm, env, args, 0);
}

static jint fg_attach_daemon(JavaVM *vm, JNIEnv **env, void *args) {
  return fg_attach_common(vm, env, args, 1);
}

static jint fg_detach(JavaVM *vm) {
  (void)vm;
  if (G) {
    atomic_fetch_add(&G->hits[232], 1);
  }
  if (gid < 0) {
    return JNI_OK;
  }
  struct fb_context *c = fb_context_at(G, gid);
  if (!c->origin || gdepth) {
    return JNI_ERR;
  }

  atomic_store(&c->closing, 1);
  fb_event_notify(&c->host_event);
  for (;;) {
    uint32_t observed = fb_event_sample(&c->guest_event);
    if (atomic_load_explicit(&c->host_done, memory_order_acquire)) {
      break;
    }
    fb_event_wait(&c->guest_event, observed);
  }

#if GW_RPC_PROFILE
  /* guest_done permits immediate slot reuse by an unrelated thread. Do not
   * update that slot's counters after publishing it, including the wake. */
  gw_rpc_current = NULL;
#endif
  atomic_store_explicit(&c->guest_done, 1, memory_order_release);
  fb_event_notify(&G->host_control);
  gid = -1;
  return JNI_OK;
}

static jint fg_destroy(JavaVM *vm) {
  (void)vm;
  if (G) {
    atomic_fetch_add(&G->hits[230], 1);
  }
  return JNI_ERR;
}

static const struct JNIInvokeInterface fg_vm_table = {
    .DestroyJavaVM = fg_destroy,
    .AttachCurrentThread = fg_attach,
    .DetachCurrentThread = fg_detach,
    .GetEnv = fg_getenv,
    .AttachCurrentThreadAsDaemon = fg_attach_daemon};
JavaVM fg_vm = &fg_vm_table;

static void *fg_worker(void *arg) {
  gid = (int)(intptr_t)arg;
  gdepth = 0;
#if GW_RPC_PROFILE
  gw_rpc_current = &guest_slot(gid, 0)->metrics;
#endif
  struct fb_context *c = fb_context_at(G, gid);
  c->guest_tid = (int)syscall(SYS_gettid);
  atomic_fetch_add(&G->live_guest_threads, 1);

  atomic_store_explicit(&c->state, FB_RUNNING, memory_order_release);
  fb_event_notify(&c->host_event);
  for (;;) {
    uint32_t observed = fb_event_sample(&c->guest_event);
    fg_pump();
    if (!atomic_load(&G->running) || atomic_load(&c->closing)) {
      break;
    }
    fb_event_wait(&c->guest_event, observed);
  }

  atomic_fetch_sub(&G->live_guest_threads, 1);
  atomic_store_explicit(&c->state, FB_DEAD, memory_order_release);
  fb_event_notify(&fb_context_at(G, 0)->guest_event);
  return NULL;
}

void fg_pump(void) {
  if (gid < 0 || gdepth < 0) {
    return;
  }

  if (gid == 0) {
    for (int i = 1; i < fb_context_capacity(G); i++) {
      struct fb_context *c = fb_context_at(G, i);
      int state = atomic_load_explicit(&c->state, memory_order_acquire);
      struct guest_slot *slot = guest_slot(i, state == FB_HOST_REQUEST);
      if (state == FB_HOST_REQUEST && !c->origin &&
          (!slot || !slot->worker_live)) {
        if (slot) {
          slot->worker_live = 1;
        }
        if (!slot || pthread_create(&slot->worker, NULL, fg_worker,
                                    (void *)(intptr_t)i)) {
          if (slot) {
            slot->worker_live = 0;
          }
          c->attach_result = JNI_ENOMEM;
          atomic_store(&c->state, FB_ATTACH_FAILED);
          atomic_fetch_add(&G->errors, 1);
          fb_event_notify(&c->host_event);
        }
      } else if (state == FB_DEAD && !c->origin && slot && slot->worker_live) {
        pthread_join(slot->worker, NULL);
        slot->worker_live = 0;
        atomic_store_explicit(&c->state, FB_JOINED, memory_order_release);
        fb_event_notify(&c->host_event);
      }
    }
  }

  struct fb_call *call = fb_call_at(fb_context_at(G, gid), gdepth);
  if (call) {
    fg_execute(call);
  }
}

void full_guest_start(struct shared *s) {
  G = (struct full_bridge *)s->full_bridge;
  struct fb_call *hello = &fb_context_at(G, 0)->call[0];
  if (!fb_loader_acknowledge(hello)) {
    dprintf(2, "LOADER_PROTOCOL_MISMATCH matching Host/layout required\n");
    atomic_store_explicit(&s->phase, -1, memory_order_release);
    syscall(SYS_exit, 95);
    __builtin_unreachable();
  }
  guest_jni_version = hello->args[11].i;
  if (hello->args[10].j != FB_JNI_HELLO ||
      !gw_jni_accepts(GW_JNI_MAX_VERSION, guest_jni_version)) {
    dprintf(2, "JNI_CAPABILITIES_MISMATCH Host capability required\n");
    atomic_store_explicit(&s->phase, -1, memory_order_release);
    syscall(SYS_exit, 95);
    __builtin_unreachable();
  }
  hello->args[12].j = FB_JNI_ACK;

#if GW_FUTEX_WAKE_FILTER
  if (fb_context_at(G, 0)->call[0].args[7].j != FB_WAKE_FILTER_HELLO) {
    dprintf(
        2,
        "WAKE_FILTER_MISMATCH Guest requires matching Host event encoding\n");
    atomic_store_explicit(&s->phase, -1, memory_order_release);
    syscall(SYS_exit, 94);
    __builtin_unreachable();
  }
  fb_context_at(G, 0)->call[0].args[8].j = FB_WAKE_FILTER_ACK;
#endif
#if GW_RPC_PROFILE
  metrics_connect(s);
#endif

  gid = 0;
  gdepth = 0;
  fb_context_at(G, 0)->guest_tid = (int)syscall(SYS_gettid);
}

void full_guest_poll(void) {
  fg_pump();
}

int full_guest_wait(struct shared *s) {
  struct fb_event *event = &fb_context_at(G, 0)->guest_event;
  uint32_t observed = fb_event_sample(event);
  fg_pump();
  if (atomic_load_explicit(&s->stop, memory_order_acquire) ||
      !atomic_load(&G->running)) {
    return 0;
  }
  fb_event_wait(event, observed);
  return 1;
}

void full_guest_shutdown(void) {
  for (int i = 1; i < fb_context_capacity(G); i++) {
    struct guest_slot *slot = guest_slot(i, 0);
    if (slot && slot->worker_live) {
      atomic_store(&fb_context_at(G, i)->closing, 1);
      fb_event_notify(&fb_context_at(G, i)->guest_event);
    }
  }

  for (int i = 1; i < fb_context_capacity(G); i++) {
    struct guest_slot *slot = guest_slot(i, 0);
    if (slot && slot->worker_live) {
      pthread_join(slot->worker, NULL);
      slot->worker_live = 0;
    }
  }

  dprintf(2, "FULL_GUEST_SHUTDOWN worker_threads=%d\n",
          atomic_load(&G->live_guest_threads));
}
