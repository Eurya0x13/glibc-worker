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
static pthread_t workers[FB_CONTEXTS];
static int worker_live[FB_CONTEXTS];
__thread uint64_t gmodule;
#if GW_RPC_PROFILE
_Thread_local struct gw_rpc_row *gw_rpc_current
    __attribute__((visibility("hidden")));
_Thread_local struct gw_rpc_scope *gw_rpc_scope_current
    __attribute__((visibility("hidden")));
static struct gw_rpc_row guest_metrics[FB_CONTEXTS];
static struct gw_rpc_stage_cost guest_clock_floor;
static struct gw_rpc_row *host_metrics;

struct gw_handoff_row *gw_rpc_peer_handoff(unsigned context) {
  return host_metrics && context < FB_CONTEXTS ? &host_metrics[context].handoff
                                               : NULL;
}

static void metrics_connect(struct shared *shared) {
  struct fb_call *hello = &G->ctx[0].call[0];

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
  host_metrics = (void *)(uintptr_t)hello->args[9].j;

  hello->args[6].j = (jlong)(uintptr_t)&guest_clock_floor;
  gw_rpc_current = &guest_metrics[0];
  hello->result.j = (jlong)(uintptr_t)guest_metrics;
  hello->error = FB_RPC_PROFILE_ACK;
  /* The owner's existing release/acquire phase-ready handshake publishes
   * this pointer. No request epoch is advanced; this is not an RPC. */
}
#endif
static void fg_pump(void);

static int fg_valid(JNIEnv *env) {
  return G && gid >= 0 && env == &fg_env;
}

static jvalue fg_rpc(JNIEnv *env, int api, jvalue *args) {
  jvalue zero = {0};
  if (!fg_valid(env) || gdepth >= FB_DEPTH) {
    if (G) {
      atomic_fetch_add(&G->errors, 1);
    }
    return zero;
  }

  GW_RPC_SCOPE(boundary, GW_RPC_STAGE_COUNT, -1);
#if GW_RPC_PROFILE
  struct gw_rpc_token metrics = gw_rpc_begin();
#endif

  GW_RPC_SCOPE(pack, GW_RPC_GUEST_RPC_PACK, -1);
  atomic_fetch_add(&G->hits[api], 1);
  struct fb_rpc *r = &G->ctx[gid].rpc[gdepth];
  memcpy(r->a, args, sizeof r->a);
  r->module = gmodule;
  r->api = api;
  r->error = 0;
  r->result = zero;
  int seq = gw_rpc_next_sequence(
      atomic_load_explicit(&r->request, memory_order_relaxed));
#if GW_RPC_PROFILE
  gw_handoff_begin(&gw_rpc_current->handoff, (unsigned)gdepth);
#endif
  atomic_store_explicit(&r->request, seq, memory_order_release);
  GW_RPC_SCOPE_END(pack);

  fb_event_notify(&G->ctx[gid].host_event);
  for (;;) {
    uint32_t observed = fb_event_sample(&G->ctx[gid].guest_event);
    fg_pump();
    if (atomic_load_explicit(&r->response, memory_order_acquire) == seq) {
      break;
    }
#if GW_RPC_READY_POLL
    fb_event_wait_active_response(&G->ctx[gid].guest_event, observed,
                                  &r->response, seq);
#else
    fb_event_wait_active(&G->ctx[gid].guest_event, observed);
#endif
  }
#if GW_RPC_PROFILE
  gw_handoff_finish(
      &gw_rpc_current->handoff, (unsigned)gdepth,
      (struct gw_handoff_record){.module = gmodule,
                                 .api = (unsigned)api,
                                 .context = (unsigned)gid,
                                 .depth = (unsigned)gdepth,
                                 .host_tid = G->ctx[gid].host_tid,
                                 .guest_tid = G->ctx[gid].guest_tid,
                                 .request = (unsigned)seq});
#endif

  if (r->error) {
    atomic_fetch_add(&G->errors, 1);
  }
#if GW_RPC_PROFILE
  gw_rpc_end(metrics);
#endif
  GW_RPC_SCOPE_END(boundary);
  return r->result;
}

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
  if (!env || !G) {
    return JNI_ERR;
  }
  *env = NULL;
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

  for (int i = 1; i < FB_CONTEXTS; i++) {
    int state = FB_FREE;
    if (!atomic_compare_exchange_strong(&G->ctx[i].state, &state,
                                        FB_RESERVED)) {
      continue;
    }
    struct fb_context *c = &G->ctx[i];
#if GW_RPC_PROFILE
    gw_rpc_current = &guest_metrics[i];
#endif
    atomic_store(&c->closing, 0);
    atomic_store(&c->host_done, 0);
    atomic_store(&c->guest_done, 0);
    atomic_store(&c->origin, 0);
    size_t payload = offsetof(struct fb_context, host_tid);
    memset((char *)c + payload, 0, sizeof *c - payload);
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
#if GW_RPC_PROFILE
        /* Retire the writer before the controller can recycle this
         * slot. The final control wake has no context metrics row. */
        gw_rpc_current = NULL;
#endif
        atomic_store_explicit(&c->guest_done, 1, memory_order_release);
        fb_event_notify(&G->host_control);
        return JNI_ERR;
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
  struct fb_context *c = &G->ctx[gid];
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
  gw_rpc_current = &guest_metrics[gid];
#endif
  struct fb_context *c = &G->ctx[gid];
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
  fb_event_notify(&G->ctx[0].guest_event);
  return NULL;
}

static void fg_pump(void) {
  if (gid < 0 || gdepth >= FB_DEPTH) {
    return;
  }

  if (gid == 0) {
    for (int i = 1; i < FB_CONTEXTS; i++) {
      struct fb_context *c = &G->ctx[i];
      int state = atomic_load_explicit(&c->state, memory_order_acquire);
      if (state == FB_HOST_REQUEST && !c->origin && !worker_live[i]) {
        worker_live[i] = 1;
        if (pthread_create(&workers[i], NULL, fg_worker, (void *)(intptr_t)i)) {
          worker_live[i] = 0;
          atomic_store(&c->state, FB_ATTACH_FAILED);
          atomic_fetch_add(&G->errors, 1);
          fb_event_notify(&c->host_event);
        }
      } else if (state == FB_DEAD && !c->origin && worker_live[i]) {
        pthread_join(workers[i], NULL);
        worker_live[i] = 0;
        atomic_store_explicit(&c->state, FB_JOINED, memory_order_release);
        fb_event_notify(&c->host_event);
      }
    }
  }

  fg_execute(&G->ctx[gid].call[gdepth]);
}

void full_guest_start(struct shared *s) {
  G = (struct full_bridge *)s->full_bridge;
  struct fb_call *hello = &G->ctx[0].call[0];
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
  if (G->ctx[0].call[0].args[7].j != FB_WAKE_FILTER_HELLO) {
    dprintf(
        2,
        "WAKE_FILTER_MISMATCH Guest requires matching Host event encoding\n");
    atomic_store_explicit(&s->phase, -1, memory_order_release);
    syscall(SYS_exit, 94);
    __builtin_unreachable();
  }
  G->ctx[0].call[0].args[8].j = FB_WAKE_FILTER_ACK;
#endif
#if GW_RPC_PROFILE
  metrics_connect(s);
#endif

  gid = 0;
  gdepth = 0;
  G->ctx[0].guest_tid = (int)syscall(SYS_gettid);
}

void full_guest_poll(void) {
  fg_pump();
}

int full_guest_wait(struct shared *s) {
  struct fb_event *event = &G->ctx[0].guest_event;
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
  for (int i = 1; i < FB_CONTEXTS; i++) {
    if (worker_live[i]) {
      atomic_store(&G->ctx[i].closing, 1);
      fb_event_notify(&G->ctx[i].guest_event);
    }
  }

  for (int i = 1; i < FB_CONTEXTS; i++) {
    if (worker_live[i]) {
      pthread_join(workers[i], NULL);
      worker_live[i] = 0;
    }
  }

  dprintf(2, "FULL_GUEST_SHUTDOWN worker_threads=%d\n",
          atomic_load(&G->live_guest_threads));
}
