#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "protocol.h"
#include "host_internal.h"
#include "dlopen_backend.h"
#include "jni_vm_adapter.h"
#include "image_policy.h"
#include "host_bridge.h"
#ifdef GW_DOMAIN_NONLOCAL
#include "nonlocal.h"
#endif

struct full_bridge *gw_host_bridge;
JavaVM *gw_host_vm;
jint gw_host_jni_version;
static struct shared *jni_shared;
static _Atomic int jni_accepted;
__thread struct host_context *gw_host_current;
static _Atomic int lifecycle = GW_HOST_NEW;
static pthread_t lifecycle_owner;

enum gw_host_state full_host_state(void) {
  return gw_dlopen_is_quarantined() ? GW_HOST_QUARANTINED
                                    : (enum gw_host_state)atomic_load_explicit(
                                          &lifecycle, memory_order_acquire);
}

static int lifecycle_on_owner(int expected) {
  return full_host_state() == (enum gw_host_state)expected &&
         pthread_equal(pthread_self(), lifecycle_owner) && gw_host_current &&
         gw_host_current->id == 0;
}

struct host_slot {
  struct host_context context;
  pthread_t helper;
  int helper_live;
#if GW_RPC_PROFILE
  struct gw_rpc_row metrics;
#endif
};

static struct host_slot *host_slot(int id, int create) {
  struct fb_context *w = fb_context_at(gw_host_bridge, id);
  if (!w) {
    return NULL;
  }

  uintptr_t value =
      atomic_load_explicit(&w->host_private, memory_order_acquire);
  if (!value && create) {
    struct host_slot *fresh = calloc(1, sizeof *fresh);
    if (!fresh) {
      return NULL;
    }
    if (atomic_compare_exchange_strong_explicit(
            &w->host_private, &value, (uintptr_t)fresh, memory_order_acq_rel,
            memory_order_acquire)) {
      value = (uintptr_t)fresh;
    } else {
      free(fresh);
    }
  }

  struct host_slot *slot = (void *)value;
#if GW_RPC_PROFILE
  if (slot) {
    atomic_store_explicit(&w->host_metrics, (uintptr_t)&slot->metrics,
                          memory_order_release);
  }
#endif
  return slot;
}

static pthread_t controller;
static pthread_key_t context_key;

jclass gw_host_helper_class;
jmethodID gw_host_describe_method, gw_host_declaring_method;
static struct gw_image_policy image_policy;
static int use_vm_adapter;
static const struct gw_jni_vm_adapter_v1 *vm_adapter;

/* Publish the negotiated JNI version into the idle bootstrap call before
   the Guest handshake; below 1.6 the bridge refuses to start. */
static int jni_prepare(struct shared *shared, JNIEnv *env) {
  gw_host_jni_version = gw_jni_negotiate((*env)->GetVersion(env));
  if (gw_host_jni_version < JNI_VERSION_1_6) {
    return JNI_ERR;
  }

  jni_shared = shared;
  struct fb_call *hello = &fb_context_at(gw_host_bridge, 0)->call[0];
  hello->args[10].j = FB_JNI_HELLO;
  hello->args[11].i = gw_host_jni_version;
  fb_loader_offer(hello);
  return JNI_OK;
}

/* One-way latch: loader activation requires the Guest's FB_JNI_ACK, proving
   both sides linked the same shared-structure ABI. */
static int jni_accept(void) {
  if (atomic_load_explicit(&jni_accepted, memory_order_acquire)) {
    return JNI_OK;
  }

  if (!gw_host_bridge || !jni_shared ||
      atomic_load_explicit(&jni_shared->phase, memory_order_acquire) != 2 ||
      fb_context_at(gw_host_bridge, 0)->call[0].args[12].j != FB_JNI_ACK) {
    fprintf(stderr,
            "JNI_CAPABILITIES_MISMATCH Guest acknowledgement required\n");
    return JNI_ERR;
  }

  atomic_store_explicit(&jni_accepted, 1, memory_order_release);
  return JNI_OK;
}

/* Registration is pre-start only: once the bridge exists the adapter is
   frozen, and a different adapter may never replace a registered one. */
int gw_jni_register_vm_adapter(const struct gw_jni_vm_adapter_v1 *adapter) {
  if (!adapter || adapter->abi_version != GW_JNI_VM_ADAPTER_V1 ||
      adapter->struct_size < sizeof *adapter || !adapter->install ||
      !adapter->restore) {
    return GW_ERROR;
  }

  if (full_host_state() != GW_HOST_NEW || gw_host_bridge ||
      (vm_adapter && vm_adapter != adapter)) {
    return GW_ERROR;
  }

  vm_adapter = adapter;
  return GW_OK;
}
#if GW_FUTEX_WAKE_FILTER
static struct shared *wake_filter_shared;
static _Atomic int wake_filter_accepted;
#endif
#if GW_RPC_PROFILE
_Thread_local struct gw_rpc_row *gw_rpc_current
    __attribute__((visibility("hidden")));
_Thread_local struct gw_rpc_scope *gw_rpc_scope_current
    __attribute__((visibility("hidden")));
/* The extra Host row belongs solely to its controller, not context zero. */
static struct gw_rpc_row controller_metrics;
static _Atomic(uintptr_t) guest_metrics;
static struct shared *metrics_shared;
static struct gw_rpc_stage_cost host_clock_floor;
static const struct gw_rpc_stage_cost *guest_clock_floor;

static struct gw_rpc_row *metric_row(int guest, unsigned context) {
  struct fb_context *w = fb_context_at(gw_host_bridge, (int)context);
  if (!w) {
    return NULL;
  }

  return (void *)atomic_load_explicit(
      guest ? &w->guest_metrics : &w->host_metrics, memory_order_acquire);
}

struct gw_handoff_row *gw_rpc_peer_handoff(unsigned context) {
  struct gw_rpc_row *row = metric_row(1, context);
  return row ? &row->handoff : NULL;
}

int gw_rpc_handoff_snapshot(struct gw_handoff_snapshot *out, size_t size) {
  if (!out || size < sizeof *out) {
    errno = EINVAL;
    return -1;
  }

  const struct gw_rpc_row *guest =
      (const void *)atomic_load_explicit(&guest_metrics, memory_order_acquire);
  if (!guest) {
    errno = ENODEV;
    return -1;
  }

  int saved = errno;
  memset(out, 0, sizeof *out);
  out->abi_version = GW_HANDOFF_ABI;
  out->struct_size = sizeof *out;
  out->contexts = fb_context_capacity(gw_host_bridge);
  out->sample_shift = GW_HANDOFF_SAMPLE_SHIFT;
  out->monotonic_ns = gw_handoff_clock(CLOCK_MONOTONIC);

  gw_handoff_accumulate(&out->host, &controller_metrics.handoff);
  for (int i = 0; i < fb_context_capacity(gw_host_bridge); i++) {
    struct gw_rpc_row *h = metric_row(0, (unsigned)i),
                      *g = metric_row(1, (unsigned)i);
    if (h) {
      gw_handoff_accumulate(&out->host, &h->handoff);
    }
    if (g) {
      gw_handoff_accumulate(&out->guest, &g->handoff);
    }
  }

  errno = saved;
  return 0;
}

int gw_rpc_diagnostics_snapshot(struct gw_rpc_snapshot *out, size_t size) {
  if (!out || size < sizeof *out) {
    errno = EINVAL;
    return -1;
  }

  const struct gw_rpc_row *guest =
      (const void *)atomic_load_explicit(&guest_metrics, memory_order_acquire);
  if (!guest) {
    errno = ENODEV;
    return -1;
  }

  int saved = errno;
  memset(out, 0, sizeof *out);
  out->abi_version = GW_RPC_METRICS_ABI;
  out->struct_size = sizeof *out;
  out->enabled = 1;
  out->contexts = fb_context_capacity(gw_host_bridge);
  out->spin_budget_ns = GW_RPC_SPIN_NS;
  out->monotonic_ns = gw_rpc_clock(NULL);

  gw_rpc_accumulate(&out->host, &controller_metrics);
  for (int i = 0; i < fb_context_capacity(gw_host_bridge); i++) {
    struct gw_rpc_row *h = metric_row(0, (unsigned)i),
                      *g = metric_row(1, (unsigned)i);
    if (h) {
      gw_rpc_accumulate(&out->host, h);
    }
    if (g) {
      gw_rpc_accumulate(&out->guest, g);
    }
  }

  errno = saved;
  return 0;
}

int gw_rpc_breakdown_snapshot(struct gw_rpc_breakdown *out, size_t size) {
  if (!out || size < sizeof *out) {
    errno = EINVAL;
    return -1;
  }

  const struct gw_rpc_row *guest =
      (const void *)atomic_load_explicit(&guest_metrics, memory_order_acquire);
  if (!guest) {
    errno = ENODEV;
    return -1;
  }

  int saved = errno;
  memset(out, 0, sizeof *out);
  out->abi_version = GW_RPC_BREAKDOWN_ABI;
  out->struct_size = sizeof *out;
  out->enabled = 1;
  out->contexts = fb_context_capacity(gw_host_bridge);
  out->sample_shift = GW_RPC_SAMPLE_SHIFT;
  out->stage_count = GW_RPC_STAGE_COUNT;
  out->api_slots = GW_RPC_API_SLOTS;
  out->monotonic_ns = gw_rpc_clock(NULL);
  out->spin_budget_ns = GW_RPC_SPIN_NS;
  out->host_clock_floor = host_clock_floor;
  out->guest_clock_floor = *guest_clock_floor;

  for (int domain = 0; domain < 2; domain++) {
    struct gw_rpc_stage_cost *stages = domain ? out->guest : out->host;
    int count = fb_context_capacity(gw_host_bridge);
    for (int i = 0; i < count + !domain; i++) {
      const struct gw_rpc_row *row =
          i == count ? &controller_metrics : metric_row(domain, (unsigned)i);
      if (!row) {
        continue;
      }
      for (unsigned stage = 0; stage < GW_RPC_STAGE_COUNT; stage++) {
        gw_rpc_cost_accumulate(&stages[stage], &row->stage[stage]);
      }
      for (unsigned outcome = 0; outcome < GW_RPC_NOTIFY_OUTCOME_COUNT;
           outcome++) {
        gw_rpc_cost_accumulate(
            domain ? &out->guest_notify[outcome] : &out->host_notify[outcome],
            &row->notify[outcome]);
      }
      if (!domain) {
        for (unsigned api = 0; api < GW_RPC_API_SLOTS; api++) {
          const struct gw_rpc_api_row *in = &row->api[api];
          struct gw_rpc_api_cost *cost = &out->api[api];
#define GW_RPC_API_SUM(name) \
  cost->name += atomic_load_explicit(&in->name, memory_order_relaxed)
          GW_RPC_API_SUM(served_calls);
          GW_RPC_API_SUM(executed_calls);
          GW_RPC_API_SUM(samples);
          GW_RPC_API_SUM(wall_ns);
          GW_RPC_API_SUM(cpu_ns);
          GW_RPC_API_SUM(clock_errors);
#undef GW_RPC_API_SUM
        }
      }
    }
  }

  for (unsigned api = 0; api < GW_RPC_API_SLOTS; api++) {
    out->api[api].existing_hits =
        atomic_load_explicit(&gw_host_bridge->hits[api], memory_order_relaxed);
  }

  errno = saved;
  return 0;
}

static void metrics_prepare(struct shared *shared) {
  metrics_shared = shared;
  gw_rpc_calibrate(&host_clock_floor);
  struct host_slot *owner = host_slot(0, 1);
  if (!owner) {
    return;
  }
  gw_rpc_current = &owner->metrics;

  struct fb_call *hello = &fb_context_at(gw_host_bridge, 0)->call[0];
  hello->op = FB_RPC_PROFILE_HELLO;
  hello->args[0].j = GW_RPC_METRICS_ABI;
  hello->args[1].j = sizeof *gw_host_bridge;
  hello->args[2].j = FB_CONTEXTS;
  hello->args[3].j = GW_RPC_SPIN_NS;
  hello->args[4].j = sizeof(struct gw_rpc_row);
  hello->args[5].j = GW_RPC_BREAKDOWN_ABI;
  hello->args[9].j = (jlong)(uintptr_t)gw_rpc_current;
}

static int metrics_accept(void) {
  if (atomic_load_explicit(&guest_metrics, memory_order_acquire)) {
    return 1;
  }
  if (!gw_host_bridge || !metrics_shared) {
    fprintf(stderr,
            "RPC_PROFILE_MISMATCH Host has not initialized the bridge\n");
    return 0;
  }

  struct fb_call *hello = &fb_context_at(gw_host_bridge, 0)->call[0];
  if (atomic_load_explicit(&metrics_shared->phase, memory_order_acquire) != 2 ||
      hello->op != FB_RPC_PROFILE_HELLO || hello->error != FB_RPC_PROFILE_ACK ||
      !hello->result.j || !hello->args[6].j) {
    fprintf(stderr,
            "RPC_PROFILE_MISMATCH Host requires matching diagnostic Guest\n");
    return 0;
  }

  guest_clock_floor = (const void *)(uintptr_t)hello->args[6].j;
  atomic_store_explicit(&guest_metrics, (uintptr_t)hello->result.j,
                        memory_order_release);
  hello->op = hello->error = 0;
  hello->result = (jvalue){0};
  return 1;
}
#endif
static uint64_t transport_current_module(JNIEnv *env) {
  (void)env;
  return gw_host_current ? gw_host_current->module : 0;
}

/* TLS exit hook for a thread that reserved a context via fh_context: ask
   its Guest worker to wind down, wait for FB_JOINED, then drop every bridge
   local and free the slot. Context zero (the launcher) and Guest-attached
   helper contexts (origin set) clean themselves up elsewhere. */
static int context_finish(struct host_context *c, int thread_exiting) {
#ifndef GW_DOMAIN_JNI
  (void)thread_exiting;
#endif
  if (gw_dlopen_is_quarantined()) {
    return EIO; /* Context/resources stay pinned. */
  }
  if (!gw_host_bridge || !atomic_load(&gw_host_bridge->running) || !c ||
      c->id == 0) {
    return 0;
  }

  struct fb_context *w = fb_context_at(gw_host_bridge, c->id);
  if (w->origin) {
    return 0;
  }

  atomic_store(&w->closing, 1);
#ifdef GW_DOMAIN_JNI
  extern int gw_domain_context_stop(struct host_context *, int);
  int stopped = gw_domain_context_stop(c, thread_exiting);
  if (stopped < 0) {
    atomic_store(&w->closing, 0);
    return -stopped;
  }
  if (!stopped) {
#endif
    fb_event_notify(&w->guest_event);
    for (;;) {
      uint32_t observed = fb_event_sample(&w->host_event);
      if (atomic_load_explicit(&w->state, memory_order_acquire) == FB_JOINED ||
          !atomic_load(&gw_host_bridge->running)) {
        break;
      }
      fb_event_wait(&w->host_event, observed);
    }
#ifdef GW_DOMAIN_JNI
  }
#endif

  fh_drop_locals(c, 0);
#if GW_RPC_PROFILE
  gw_rpc_current = NULL;
#endif
  atomic_store_explicit(&w->state, FB_FREE, memory_order_release);
  gw_host_current = NULL;
  return 0;
}

static void context_destructor(void *ptr) {
  int error = context_finish(ptr, 1);
#ifdef GW_DOMAIN_NONLOCAL
  if (!error) {
    error = gw_nonlocal_finish_thread();
  }
#endif
  if (error) {
    gw_dlopen_quarantine();
  }
}

#ifdef GW_DOMAIN_JNI
/* The standalone VM fixture releases while its caller is still attached.
 * Ordinary Java-created threads use the existing pthread-key destructor. */
int gw_domain_release_context(void) {
  struct host_context *c = gw_host_current;
  int error = context_finish(c, 0);
  if (!error) {
    pthread_setspecific(context_key, NULL);
  }
  return error;
}
#endif

/* Bind the calling JNIEnv to a bridge context, reserving one on first use.
   The CAS claims the slot; per-use fields are reset but the event epochs
   are preserved (see fb_context). The Guest controller must then spawn the
   matching worker: we wait for FB_RUNNING and fail on FB_ATTACH_FAILED.
   Failure preserves any pending exception, otherwise raises a typed Java
   error; loader callers also receive a structured status. */
static struct host_context *context_failure(JNIEnv *env,
                                            struct gw_loader_error *error,
                                            int code, const char *message) {
  gw_loader_fail(error, code, message);
  fh_throw(env, code == GW_E_CAPACITY ? FB_FAILURE_MEMORY : FB_FAILURE_STATE,
           message);
  if (gw_host_bridge) {
    atomic_fetch_add(&gw_host_bridge->errors, 1);
  }
  return NULL;
}

struct host_context *fh_context(JNIEnv *env, struct gw_loader_error *error) {
  if (gw_dlopen_is_quarantined()) {
    return context_failure(env, error, GW_E_QUARANTINED,
                           "Guest bridge quarantined; discard process");
  }
  if (!gw_host_bridge || !atomic_load(&gw_host_bridge->running)) {
    return context_failure(env, error, GW_E_NOT_READY,
                           "JNI bridge is not running");
  }
  if (gw_host_current) {
    gw_host_current->env = env;
    return gw_host_current;
  }

  int i = fb_reserve_context(gw_host_bridge);
  if (i >= 0) {
    struct fb_context *w = fb_context_at(gw_host_bridge, i);
    struct host_slot *slot = host_slot(i, 1);
    if (!slot) {
      atomic_store_explicit(&w->state, FB_FREE, memory_order_release);
      return context_failure(env, error, GW_E_CAPACITY,
                             "Cannot allocate Host context");
    }

    fb_context_reset(w);
    w->host_tid = (int)syscall(SYS_gettid);
    w->origin = 0;

    struct host_context *c = &host_slot(i, 0)->context;
    memset(c, 0, sizeof *c);
    c->id = i;
    c->env = env;
    gw_host_current = c;
#if GW_RPC_PROFILE
    gw_rpc_current = &host_slot(i, 0)->metrics;
#endif
    if (pthread_setspecific(context_key, c)) {
      gw_host_current = NULL;
#if GW_RPC_PROFILE
      gw_rpc_current = NULL;
#endif
      atomic_store_explicit(&w->state, FB_FREE, memory_order_release);
      return context_failure(env, error, GW_E_CAPACITY,
                             "Cannot allocate Host thread-local context");
    }

#ifdef GW_DOMAIN_JNI
    extern int gw_domain_context_start(struct host_context *);
    if (!gw_domain_context_start(c)) {
#endif
      atomic_store_explicit(&w->state, FB_HOST_REQUEST, memory_order_release);
      fb_owner_post(gw_host_bridge, i);
#ifdef GW_DOMAIN_JNI
    }
#endif
    for (;;) {
      uint32_t observed = fb_event_sample(&w->host_event);
      int attached = atomic_load_explicit(&w->state, memory_order_acquire);
      if (attached == FB_RUNNING) {
        break;
      }
      if (attached == FB_ATTACH_FAILED) {
        pthread_setspecific(context_key, NULL);
        gw_host_current = NULL;
#if GW_RPC_PROFILE
        gw_rpc_current = NULL;
#endif
        int status = w->attach_result;
        atomic_store_explicit(&w->state, FB_FREE, memory_order_release);
        return context_failure(
            env, error, status == JNI_ENOMEM ? GW_E_CAPACITY : GW_E_BRIDGE,
            "Cannot start Guest worker for Host thread");
      }
      fb_event_wait(&w->host_event, observed);
    }
    return c;
  }

  return context_failure(env, error, GW_E_CAPACITY,
                         "JNI bridge context capacity exhausted");
}

/* Thread body for a Guest-requested context: attach to the VM (as daemon
   when asked), service inbound JNI rpc until closing or shutdown, then
   detach. PushLocalFrame bounds every local created while attached;
   PopLocalFrame(NULL) discards them all before DetachCurrentThread. */
static void *attached_helper(void *opaque) {
  int i = (int)(intptr_t)opaque;
  struct fb_context *w = fb_context_at(gw_host_bridge, i);
  struct host_context *c = &host_slot(i, 0)->context;
  memset(c, 0, sizeof *c);
  c->id = i;
  gw_host_current = c;
#if GW_RPC_PROFILE
  gw_rpc_current = &host_slot(i, 0)->metrics;
#endif

  JNIEnv *env = NULL;
  JavaVMAttachArgs args = {.version = JNI_VERSION_1_6,
                           .name = w->name[0] ? w->name : NULL,
                           .group = NULL};
  jint rc =
      w->daemon
          ? (*gw_host_vm)->AttachCurrentThreadAsDaemon(gw_host_vm, &env, &args)
          : (*gw_host_vm)->AttachCurrentThread(gw_host_vm, &env, &args);

  if (rc != JNI_OK) {
    w->attach_result = rc;
    atomic_store(&w->state, FB_ATTACH_FAILED);
    atomic_store_explicit(&w->host_done, 1, memory_order_release);
    fb_event_notify(&w->guest_event);
    gw_host_current = NULL;
#if GW_RPC_PROFILE
    gw_rpc_current = NULL;
#endif
    return NULL;
  }

  c->env = env;
  w->host_tid = (int)syscall(SYS_gettid);
  if ((*env)->PushLocalFrame(env, 1024) != JNI_OK) {
    if ((*env)->ExceptionCheck(env)) {
      (*env)->ExceptionClear(env);
    }
    (*gw_host_vm)->DetachCurrentThread(gw_host_vm);
    w->attach_result = JNI_ENOMEM;
    atomic_store_explicit(&w->state, FB_ATTACH_FAILED, memory_order_release);
    atomic_store_explicit(&w->host_done, 1, memory_order_release);
    fb_event_notify(&w->guest_event);
    gw_host_current = NULL;
#if GW_RPC_PROFILE
    gw_rpc_current = NULL;
#endif
    return NULL;
  }
  c->local_depth = 1;
  atomic_store_explicit(&w->state, FB_RUNNING, memory_order_release);
  fb_event_notify(&w->guest_event);

  for (;;) {
    uint32_t observed = fb_event_sample(&w->host_event);
    full_host_poll();
    if (!atomic_load(&gw_host_bridge->running) || atomic_load(&w->closing)) {
      break;
    }
    fb_event_wait(&w->host_event, observed);
  }

  (*env)->PopLocalFrame(env, NULL);
  fh_drop_locals(c, 0);
  (*gw_host_vm)->DetachCurrentThread(gw_host_vm);
  atomic_store_explicit(&w->host_done, 1, memory_order_release);
  fb_event_notify(&w->guest_event);
  fb_event_notify(&gw_host_bridge->host_control);

  gw_host_current = NULL;
#if GW_RPC_PROFILE
  gw_rpc_current = NULL;
#endif
  return NULL;
}

/* Owns helper creation/reaping even while every JRE caller is asleep in Java.
   This bionic thread never invokes JNI; attached helpers own their own JNIEnv.
 */
static void *host_controller(void *unused) {
  (void)unused;
#if GW_RPC_PROFILE
  gw_rpc_current = &controller_metrics;
#endif

  for (;;) {
    uint32_t observed = fb_event_sample(&gw_host_bridge->host_control);
    if (!atomic_load(&gw_host_bridge->running)) {
      break;
    }
    for (int i = 1; i < fb_context_capacity(gw_host_bridge); i++) {
      struct fb_context *w = fb_context_at(gw_host_bridge, i);
      int state = FB_GUEST_ATTACH;
      if (atomic_compare_exchange_strong(&w->state, &state,
                                         FB_ATTACH_STARTING)) {
        struct host_slot *slot = host_slot(i, 1);
        if (slot) {
          slot->helper_live = 1;
        }
        if (!slot || pthread_create(&slot->helper, NULL, attached_helper,
                                    (void *)(intptr_t)i)) {
          if (slot) {
            slot->helper_live = 0;
          }
          w->attach_result = JNI_ENOMEM;
          atomic_store(&w->state, FB_ATTACH_FAILED);
          atomic_store_explicit(&w->host_done, 1, memory_order_release);
          atomic_fetch_add(&gw_host_bridge->errors, 1);
          fb_event_notify(&w->guest_event);
        }
      } else if ((state == FB_RUNNING || state == FB_ATTACH_FAILED) &&
                 w->origin && atomic_load(&w->host_done) &&
                 atomic_load(&w->guest_done)) {
        struct host_slot *slot = host_slot(i, 0);
        if (slot && slot->helper_live) {
          pthread_join(slot->helper, NULL);
          slot->helper_live = 0;
        }
        atomic_store_explicit(&w->state, FB_FREE, memory_order_release);
      }
    }
    if (atomic_load(&gw_host_bridge->running)) {
      fb_event_wait(&gw_host_bridge->host_control, observed);
    }
  }

  return NULL;
}

/* Drain pending Guest->Host JNI requests on the current context's rpc
   slots. Invoked while blocked in fh_command and from helper wait loops;
   without it a Host thread waiting on the Guest could not answer callbacks
   arriving on its own context. */
void full_host_poll(void) {
  if (!gw_host_bridge) {
    return;
  }

  if (gw_host_current) {
    struct fb_context *w = fb_context_at(gw_host_bridge, gw_host_current->id);
    struct fb_rpc *emergency = &w->emergency;
    if (atomic_load_explicit(&emergency->request, memory_order_acquire) !=
            atomic_load(&emergency->response) &&
        !atomic_load(&emergency->active)) {
      fh_service(gw_host_current, emergency);
    }
    int top = atomic_load_explicit(&w->rpc_top, memory_order_acquire);
    struct fb_rpc *r = fb_rpc_at(w, top);
    if (r &&
        atomic_load_explicit(&r->request, memory_order_acquire) !=
            atomic_load(&r->response) &&
        !atomic_load(&r->active)) {
      fh_service(gw_host_current, r);
    }
  }
}

/* One-time bridge bring-up from the launcher thread: validate the config,
   allocate the zeroed shared bridge (refuses re-init in-process), pin
   BridgeTypes, configure the loader transports and image policy, install
   the legacy adapter when requested, and start the controller. The Guest
   handshake must still pass full_host_ready before any load is allowed. */
static int start_bridge(struct shared *s, JNIEnv *env,
                        const struct gw_host_bridge_config *options) {
  const char *dir = options->native_directory;
  use_vm_adapter = (options->flags & GW_HOST_VM_ADAPTER) != 0;
  fh_array_copy_start((options->flags & GW_HOST_DISABLE_ARRAY_COPY_CACHE) == 0);
  printf("BINDING_REGISTRY backend=%s\n", fh_binding_backend());

  gw_host_bridge = mmap(NULL, sizeof *gw_host_bridge, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (gw_host_bridge == MAP_FAILED) {
    gw_host_bridge = NULL;
    fh_throw(env, FB_FAILURE_MEMORY, "Cannot map JNI bridge shared state");
    return JNI_ERR;
  }
  s->full_bridge = (uintptr_t)gw_host_bridge;
  if (jni_prepare(s, env) != JNI_OK) {
    return JNI_ERR;
  }

#if GW_RPC_PROFILE
  metrics_prepare(s);
#endif
#if GW_FUTEX_WAKE_FILTER
  wake_filter_shared = s;
  fb_context_at(gw_host_bridge, 0)->call[0].args[7].j = FB_WAKE_FILTER_HELLO;
#endif

  if ((*env)->GetJavaVM(env, &gw_host_vm) != JNI_OK) {
    return JNI_ERR;
  }
  if (pthread_key_create(&context_key, context_destructor)) {
    fh_throw(env, FB_FAILURE_MEMORY, "Cannot allocate JNI context TLS key");
    return JNI_ERR;
  }

  struct host_slot *owner = host_slot(0, 1);
  struct fb_context *owner_wire = fb_context_at(gw_host_bridge, 0);
  if (!owner || !owner_wire) {
    fh_throw(env, FB_FAILURE_MEMORY, "Cannot allocate initial JNI context");
    return JNI_ERR;
  }
  fb_context_reset(owner_wire);
  owner->context = (struct host_context){.env = env, .id = 0};
  gw_host_current = &owner->context;
  if (pthread_setspecific(context_key, gw_host_current)) {
    gw_host_current = NULL;
    fh_throw(env, FB_FAILURE_MEMORY, "Cannot bind initial JNI context");
    return JNI_ERR;
  }
  owner_wire->host_tid = (int)syscall(SYS_gettid);
  atomic_store(&owner_wire->state, FB_RUNNING);
  atomic_store(&gw_host_bridge->running, 1);

  jclass cls = (*env)->FindClass(env, "dev/glibc/worker/BridgeTypes");
  if (!cls) {
    return JNI_ERR;
  }
  gw_host_helper_class = (jclass)(*env)->NewGlobalRef(env, cls);
  (*env)->DeleteLocalRef(env, cls);
  if (!gw_host_helper_class) {
    return JNI_ERR;
  }

  gw_host_describe_method =
      (*env)->GetStaticMethodID(env, gw_host_helper_class, "describeMember",
                                "(Ljava/lang/Object;)Ljava/lang/String;");
  if (!gw_host_describe_method) {
    return JNI_ERR;
  }

  gw_host_declaring_method =
      (*env)->GetStaticMethodID(env, gw_host_helper_class, "declaringClass",
                                "(Ljava/lang/Object;)Ljava/lang/Class;");
  if (!gw_host_declaring_method) {
    return JNI_ERR;
  }

  struct gw_loader_error error = {0};
  if (gw_image_policy_start(&image_policy, env, dir, options->image_catalog,
                            (options->flags & GW_HOST_DIRECT_IMAGES) != 0,
                            &error) != GW_OK) {
    goto fail;
  }

  struct gw_loader_config config = {
      .transport = {.load = fh_transport_load,
                    .close = fh_transport_close,
                    .find = fh_transport_find,
                    .current_module = transport_current_module},
      .resolve_image = gw_image_policy_resolve,
      .image_context = &image_policy,
      .counters = gw_host_bridge};
  if (gw_loader_configure(&config, &error) != GW_OK) {
    goto fail;
  }

  const char *jre_path = options->abi_version == GW_HOST_BRIDGE_CONFIG_V2
                             ? options->jre_library_path
                             : NULL;
  if (use_vm_adapter &&
      vm_adapter->install(env, dir, jre_path, &error) != GW_OK) {
    goto fail;
  }

  if (pthread_create(&controller, NULL, host_controller, NULL)) {
    gw_loader_fail(&error, GW_E_BRIDGE, "Cannot create Host controller");
    goto fail;
  }

  return JNI_OK;
fail:
  printf("LOADER_BACKEND initialization_failed code=%d message=%s\n",
         error.code, error.message);
  return JNI_ERR;
}

/* Claim the process once, before publishing any bridge state. Validation and
   missing-adapter errors leave NEW; failures after the claim retain resources
   and require process disposal rather than a partially initialized shutdown. */
int full_host_start(struct shared *s, JNIEnv *env,
                    const struct gw_host_bridge_config *options) {
  if (!s || !env || !options ||
      (options->abi_version != GW_HOST_BRIDGE_CONFIG_V1 &&
       options->abi_version != GW_HOST_BRIDGE_CONFIG_V2) ||
      options->struct_size < GW_HOST_BRIDGE_CONFIG_V1_SIZE ||
      (options->abi_version == GW_HOST_BRIDGE_CONFIG_V2 &&
       options->struct_size < sizeof *options) ||
      (options->flags &
       ~(GW_HOST_VM_ADAPTER | GW_HOST_DIRECT_IMAGES | GW_HOST_ARRAY_COPY_CACHE |
         GW_HOST_DISABLE_ARRAY_COPY_CACHE)) ||
      !options->native_directory || !options->image_catalog) {
    return JNI_ERR;
  }

  if ((options->flags & GW_HOST_VM_ADAPTER) && !vm_adapter) {
    fprintf(stderr, "JNI adapter requested but jvm-jni is not registered\n");
    return JNI_ERR;
  }
  int expected = GW_HOST_NEW;
  if (gw_dlopen_is_quarantined() ||
      !atomic_compare_exchange_strong_explicit(
          &lifecycle, &expected, GW_HOST_STARTING, memory_order_acq_rel,
          memory_order_acquire)) {
    return JNI_ERR;
  }
  lifecycle_owner = pthread_self();
  int result = start_bridge(s, env, options);
  if (result != JNI_OK) {
    gw_dlopen_quarantine();
  } else {
    atomic_store_explicit(&lifecycle, GW_HOST_STARTED, memory_order_release);
  }
  return result;
}

/* Handshake gate once the Guest reached phase 2: verify the JNI (and any
   optional wake-filter/profile) acknowledgements, then activate the loader
   so dlopen interception begins. */
static int accept_guest(void) {
  if (jni_accept() != JNI_OK) {
    return JNI_ERR;
  }
  if (!fb_loader_accepted(&fb_context_at(gw_host_bridge, 0)->call[0])) {
    fprintf(stderr,
            "LOADER_PROTOCOL_MISMATCH matching layout/module capacity "
            "acknowledgement "
            "required\n");
    return JNI_ERR;
  }
  printf("LOADER_PROTOCOL version=%d matched=1\n", FB_LOADER_PROTOCOL);
#if GW_FUTEX_WAKE_FILTER
  if (!atomic_load_explicit(&wake_filter_accepted, memory_order_acquire)) {
    if (!gw_host_bridge || !wake_filter_shared ||
        atomic_load_explicit(&wake_filter_shared->phase,
                             memory_order_acquire) != 2 ||
        fb_context_at(gw_host_bridge, 0)->call[0].args[8].j !=
            FB_WAKE_FILTER_ACK) {
      fprintf(
          stderr,
          "WAKE_FILTER_MISMATCH Host requires matching Guest event encoding\n");
      return JNI_ERR;
    }
    atomic_store_explicit(&wake_filter_accepted, 1, memory_order_release);
  }
#endif

#if GW_RPC_PROFILE
  if (!metrics_accept()) {
    return JNI_ERR;
  }
#endif

  struct gw_loader_error error = {0};
  int rc = gw_loader_activate(&error);
  if (rc != GW_OK) {
    printf("LOADER_BACKEND activation_failed=%s\n", error.message);
  }
  return rc == GW_OK ? JNI_OK : JNI_ERR;
}

int full_host_ready(void) {
  if (!lifecycle_on_owner(GW_HOST_STARTED)) {
    return JNI_ERR;
  }

  int result = accept_guest();

  if (result == JNI_OK) {
    atomic_store_explicit(&lifecycle, GW_HOST_READY, memory_order_release);
  } else {
    gw_dlopen_quarantine();
  }
  return result;
}

static int shutdown_requests_idle(struct fb_context *w) {
  if (atomic_load(&w->emergency.active) ||
      atomic_load(&w->emergency.request) !=
          atomic_load(&w->emergency.response)) {
    return 0;
  }

  for (int depth = 0; depth < fb_frame_capacity(w); depth++) {
    struct fb_call *call = fb_call_at(w, depth);
    struct fb_rpc *rpc = fb_rpc_at(w, depth);
    if (atomic_load(&call->request) != atomic_load(&call->response) ||
        atomic_load(&rpc->active) ||
        atomic_load(&rpc->request) != atomic_load(&rpc->response)) {
      return 0;
    }
  }

  return 1;
}

/* Idle Host bindings may outlive the bridge (e.g. VM reference-handler
   threads); their TLS/private mappings remain pinned. Guest attachments must
   detach and be reaped. FB_JOINED alone is insufficient: the Host destructor
   may still be releasing references. The owner must already have stopped
   producers; this snapshot is not a concurrent admission gate. */
static int shutdown_contexts_idle(void) {
  if (gw_host_current->depth || gw_host_current->module ||
      gw_host_current->emergency) {
    fprintf(stderr, "JNI shutdown refused: owner still executing\n");
    return 0;
  }

  for (int i = 0; i < fb_context_capacity(gw_host_bridge); i++) {
    struct fb_context *w = fb_context_at(gw_host_bridge, i);
    int state = atomic_load_explicit(&w->state, memory_order_acquire);
    if ((state != FB_FREE && (state != FB_RUNNING || atomic_load(&w->origin) ||
                              atomic_load(&w->closing))) ||
        !shutdown_requests_idle(w)) {
      fprintf(stderr,
              "JNI shutdown refused: context=%d state=%d origin=%d "
              "host_done=%d guest_done=%d\n",
              i, atomic_load(&w->state), atomic_load(&w->origin),
              atomic_load(&w->host_done), atomic_load(&w->guest_done));
      return 0;
    }
  }

  return 1;
}

/* Teardown mirrors full_host_start on the launcher thread: the loader must
   stop cleanly (it refuses while guest images remain live), helpers are
   woken and joined, and the array cache, ID cache, and pinned refs are
   released. The bridge mapping is left in place on purpose — gw_host_bridge
   stays set so a second start is refused. */
int full_host_shutdown(void) {
  if (!lifecycle_on_owner(GW_HOST_READY) || !shutdown_contexts_idle()) {
    return JNI_ERR;
  }
  atomic_store_explicit(&lifecycle, GW_HOST_STOPPING, memory_order_release);
  JNIEnv *env = gw_host_current->env;
  struct gw_loader_error error = {0};
  if (gw_loader_stop(&error) != GW_OK) {
    printf("LOADER_BACKEND teardown_refused=%s\n", error.message);
    if (error.code == GW_E_BUSY) {
      atomic_store_explicit(&lifecycle, GW_HOST_READY, memory_order_release);
    } else {
      gw_dlopen_quarantine();
    }
    return JNI_ERR;
  }

  if (use_vm_adapter && vm_adapter->restore(env, &error) != GW_OK) {
    printf("LOADER_BACKEND adapter_restore_failed=%s\n", error.message);
    gw_dlopen_quarantine();
    return JNI_ERR;
  }

  gw_image_policy_finish(&image_policy, env);
  atomic_store(&gw_host_bridge->running, 0);
  fb_event_notify(&gw_host_bridge->host_control);
  for (int i = 0; i < fb_context_capacity(gw_host_bridge); i++) {
    fb_event_notify(&fb_context_at(gw_host_bridge, i)->host_event);
    fb_event_notify(&fb_context_at(gw_host_bridge, i)->guest_event);
  }

  if (pthread_join(controller, NULL)) {
    gw_dlopen_quarantine();
    return JNI_ERR;
  }
  for (int i = 1; i < fb_context_capacity(gw_host_bridge); i++) {
    struct host_slot *slot = host_slot(i, 0);
    if (slot && slot->helper_live) {
      if (pthread_join(slot->helper, NULL)) {
        gw_dlopen_quarantine();
        return JNI_ERR;
      }
      slot->helper_live = 0;
    }
  }
  fh_array_copy_finish();
  fh_bindings_finish();
  sweep_ids(env, 1);
  if (gw_host_helper_class) {
    (*env)->DeleteGlobalRef(env, gw_host_helper_class);
    gw_host_helper_class = NULL;
  }
  pthread_setspecific(context_key, NULL);
  pthread_key_delete(context_key);
  gw_host_current = NULL;
  atomic_store_explicit(&lifecycle, GW_HOST_STOPPED, memory_order_release);
  printf("FULL_HOST_SHUTDOWN helpers_joined=1 id_cache_weak=0\n");
  return JNI_OK;
}
