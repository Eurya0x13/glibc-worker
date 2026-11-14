#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "protocol.h"
#include "host_internal.h"
#include "dlopen_backend.h"
#include "jni_vm_adapter.h"
#include "image_policy.h"

struct full_bridge *gw_host_bridge;
JavaVM *gw_host_vm;
jint gw_host_jni_version;
static struct shared *jni_shared;
static _Atomic int jni_accepted;
__thread struct host_context *gw_host_current;
static struct host_context contexts[FB_CONTEXTS];
static pthread_t controller;
static pthread_key_t context_key;
static pthread_t helpers[FB_CONTEXTS];
static int helper_live[FB_CONTEXTS];
jclass gw_host_helper_class;
jmethodID gw_host_describe_method, gw_host_declaring_method;
static struct gw_image_policy image_policy;
static int legacy_adapter;
static const struct gw_jni_vm_adapter_v1 *vm_adapter;

/* Publish the negotiated JNI version into the idle bootstrap call before
   the Guest handshake; below 1.6 the bridge refuses to start. */
static int jni_prepare(struct shared *shared, JNIEnv *env) {
  gw_host_jni_version = gw_jni_negotiate((*env)->GetVersion(env));
  if (gw_host_jni_version < JNI_VERSION_1_6) {
    return JNI_ERR;
  }
  jni_shared = shared;
  struct fb_call *hello = &gw_host_bridge->ctx[0].call[0];
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
      gw_host_bridge->ctx[0].call[0].args[12].j != FB_JNI_ACK) {
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

  if (gw_host_bridge || (vm_adapter && vm_adapter != adapter)) {
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
static struct gw_rpc_row host_metrics[FB_CONTEXTS + 1];
static _Atomic(uintptr_t) guest_metrics;
static struct shared *metrics_shared;
static struct gw_rpc_stage_cost host_clock_floor;
static const struct gw_rpc_stage_cost *guest_clock_floor;

struct gw_handoff_row *gw_rpc_peer_handoff(unsigned context) {
  struct gw_rpc_row *rows =
      (void *)atomic_load_explicit(&guest_metrics, memory_order_acquire);
  return rows && context < FB_CONTEXTS ? &rows[context].handoff : NULL;
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
  out->contexts = FB_CONTEXTS;
  out->sample_shift = GW_HANDOFF_SAMPLE_SHIFT;
  out->monotonic_ns = gw_handoff_clock(CLOCK_MONOTONIC);

  for (int i = 0; i <= FB_CONTEXTS; i++) {
    gw_handoff_accumulate(&out->host, &host_metrics[i].handoff);
  }
  for (int i = 0; i < FB_CONTEXTS; i++) {
    gw_handoff_accumulate(&out->guest, &guest[i].handoff);
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
  out->contexts = FB_CONTEXTS;
  out->spin_budget_ns = GW_RPC_SPIN_NS;
  out->monotonic_ns = gw_rpc_clock(NULL);

  for (int i = 0; i <= FB_CONTEXTS; i++) {
    gw_rpc_accumulate(&out->host, &host_metrics[i]);
  }
  for (int i = 0; i < FB_CONTEXTS; i++) {
    gw_rpc_accumulate(&out->guest, &guest[i]);
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
  out->contexts = FB_CONTEXTS;
  out->sample_shift = GW_RPC_SAMPLE_SHIFT;
  out->stage_count = GW_RPC_STAGE_COUNT;
  out->api_slots = GW_RPC_API_SLOTS;
  out->monotonic_ns = gw_rpc_clock(NULL);
  out->spin_budget_ns = GW_RPC_SPIN_NS;
  out->host_clock_floor = host_clock_floor;
  out->guest_clock_floor = *guest_clock_floor;

  for (int domain = 0; domain < 2; domain++) {
    const struct gw_rpc_row *rows = domain ? guest : host_metrics;
    struct gw_rpc_stage_cost *stages = domain ? out->guest : out->host;
    for (int i = 0; i < FB_CONTEXTS + !domain; i++) {
      for (unsigned stage = 0; stage < GW_RPC_STAGE_COUNT; stage++) {
        gw_rpc_cost_accumulate(&stages[stage], &rows[i].stage[stage]);
      }
      for (unsigned outcome = 0; outcome < GW_RPC_NOTIFY_OUTCOME_COUNT;
           outcome++) {
        gw_rpc_cost_accumulate(
            domain ? &out->guest_notify[outcome] : &out->host_notify[outcome],
            &rows[i].notify[outcome]);
      }
      if (!domain) {
        for (unsigned api = 0; api < GW_RPC_API_SLOTS; api++) {
          const struct gw_rpc_api_row *in = &rows[i].api[api];
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
  gw_rpc_current = &host_metrics[0];

  struct fb_call *hello = &gw_host_bridge->ctx[0].call[0];
  hello->op = FB_RPC_PROFILE_HELLO;
  hello->args[0].j = GW_RPC_METRICS_ABI;
  hello->args[1].j = sizeof *gw_host_bridge;
  hello->args[2].j = FB_CONTEXTS;
  hello->args[3].j = GW_RPC_SPIN_NS;
  hello->args[4].j = sizeof(struct gw_rpc_row);
  hello->args[5].j = GW_RPC_BREAKDOWN_ABI;
  hello->args[9].j = (jlong)(uintptr_t)host_metrics;
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

  struct fb_call *hello = &gw_host_bridge->ctx[0].call[0];
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

/* Serve one Guest->Host JNI request. The request/response sequence pair
   plus the "active" exchange guarantee a single server per slot even when a
   stale notify re-runs the pump. errno is saved and restored because the
   dispatched JNI code must not perturb the Host caller's errno. The
   context's module is switched to r->module so any load triggered by the
   call is attributed to the requesting guest image. */
void fh_service(struct host_context *c, struct fb_rpc *r) {
  JNIEnv *env = c->env;
  int seq = atomic_load_explicit(&r->request, memory_order_acquire);
  if (seq == atomic_load(&r->response) || atomic_exchange(&r->active, 1)) {
    return;
  }

  int saved = errno;
#if GW_RPC_PROFILE
  unsigned handoff_depth = (unsigned)(r - gw_host_bridge->ctx[c->id].rpc);
  struct gw_handoff_row *handoff = gw_rpc_peer_handoff((unsigned)c->id);
  if (handoff_depth < FB_DEPTH) {
    gw_handoff_dispatch(handoff, handoff_depth);
  }
#endif
  GW_RPC_SCOPE(glue, GW_RPC_HOST_JNI_GLUE, -1);
#if GW_RPC_PROFILE
  if (r->api >= 0) {
    gw_rpc_served((unsigned)r->api);
  }
#endif

  uint64_t previous = c->module;
  c->module = r->module;
  if (r->api == 14) {
    printf(
        "JNI_EXPECTED_FATAL stage=host-forward index=14 name=FatalError "
        "calls=%u\n",
        atomic_load(&gw_host_bridge->hits[14]));
  }
  int previous_marshal_error = c->marshal_error;
  c->marshal_error = 0;
  if (r->api == FB_RPC_REPORT_FAILURE) {
    enum fb_failure kind = r->a[0].i;
    if (kind < FB_FAILURE_MEMORY || kind > FB_FAILURE_STATE) {
      kind = FB_FAILURE_STATE;
    }
    fh_throw(env, kind,
             kind == FB_FAILURE_DEPTH ? "Guest bridge nesting limit exceeded"
                                      : "Guest bridge request rejected");
    r->error = kind;
    r->result.i = JNI_ERR;
  } else {
#include "generated/host-jni.inc"
  }
  if (c->marshal_error && !r->error) {
    r->error = c->marshal_error;
  }
  c->marshal_error = previous_marshal_error;
  if (r->error) {
    fh_throw(env, FB_FAILURE_STATE, "JNI bridge request failed");
    atomic_fetch_add(&gw_host_bridge->errors, 1);
  }
  c->module = previous;
  GW_RPC_SCOPE_END(glue);
  errno = saved;
  atomic_store(&r->active, 0);

#if GW_RPC_PROFILE
  if (handoff_depth < FB_DEPTH) {
    gw_handoff_complete(handoff, handoff_depth);
  }
#endif
  atomic_store_explicit(&r->response, seq, memory_order_release);
  fb_event_notify(&gw_host_bridge->ctx[c->id].guest_event);
}

/* TLS exit hook for a thread that reserved a context via fh_context: ask
   its Guest worker to wind down, wait for FB_JOINED, then drop every bridge
   local and free the slot. Context zero (the launcher) and Guest-attached
   helper contexts (origin set) clean themselves up elsewhere. */
static void context_destructor(void *ptr) {
  struct host_context *c = ptr;
  if (gw_dlopen_is_quarantined()) {
    return; /* Context/resources stay pinned. */
  }
  if (!gw_host_bridge || !atomic_load(&gw_host_bridge->running) || !c ||
      c->id == 0) {
    return;
  }

  struct fb_context *w = &gw_host_bridge->ctx[c->id];
  if (w->origin) {
    return;
  }

  atomic_store(&w->closing, 1);
  fb_event_notify(&w->guest_event);
  for (;;) {
    uint32_t observed = fb_event_sample(&w->host_event);
    if (atomic_load_explicit(&w->state, memory_order_acquire) == FB_JOINED ||
        !atomic_load(&gw_host_bridge->running)) {
      break;
    }
    fb_event_wait(&w->host_event, observed);
  }

  fh_drop_locals(c, 0);
#if GW_RPC_PROFILE
  gw_rpc_current = NULL;
#endif
  atomic_store_explicit(&w->state, FB_FREE, memory_order_release);
  gw_host_current = NULL;
}

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

  for (int i = 1; i < FB_CONTEXTS; i++) {
    int state = FB_FREE;
    struct fb_context *w = &gw_host_bridge->ctx[i];
    if (!atomic_compare_exchange_strong(&w->state, &state, FB_RESERVED)) {
      continue;
    }

    atomic_store(&w->closing, 0);
    atomic_store(&w->host_done, 0);
    atomic_store(&w->guest_done, 0);
    atomic_store(&w->origin, 0);
    size_t payload = offsetof(struct fb_context, host_tid);
    memset((char *)w + payload, 0, sizeof *w - payload);
    w->host_tid = (int)syscall(SYS_gettid);
    w->origin = 0;

    struct host_context *c = &contexts[i];
    memset(c, 0, sizeof *c);
    c->id = i;
    c->env = env;
    gw_host_current = c;
#if GW_RPC_PROFILE
    gw_rpc_current = &host_metrics[i];
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

    atomic_store_explicit(&w->state, FB_HOST_REQUEST, memory_order_release);
    fb_event_notify(&gw_host_bridge->ctx[0].guest_event);
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
  struct fb_context *w = &gw_host_bridge->ctx[i];
  struct host_context *c = &contexts[i];
  memset(c, 0, sizeof *c);
  c->id = i;
  gw_host_current = c;
#if GW_RPC_PROFILE
  gw_rpc_current = &host_metrics[i];
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
  gw_rpc_current = &host_metrics[FB_CONTEXTS];
#endif

  for (;;) {
    uint32_t observed = fb_event_sample(&gw_host_bridge->host_control);
    if (!atomic_load(&gw_host_bridge->running)) {
      break;
    }
    for (int i = 1; i < FB_CONTEXTS; i++) {
      struct fb_context *w = &gw_host_bridge->ctx[i];
      int state = FB_GUEST_ATTACH;
      if (atomic_compare_exchange_strong(&w->state, &state,
                                         FB_ATTACH_STARTING)) {
        helper_live[i] = 1;
        if (pthread_create(&helpers[i], NULL, attached_helper,
                           (void *)(intptr_t)i)) {
          helper_live[i] = 0;
          w->attach_result = JNI_ENOMEM;
          atomic_store(&w->state, FB_ATTACH_FAILED);
          atomic_store_explicit(&w->host_done, 1, memory_order_release);
          atomic_fetch_add(&gw_host_bridge->errors, 1);
          fb_event_notify(&w->guest_event);
        }
      } else if ((state == FB_RUNNING || state == FB_ATTACH_FAILED) &&
                 w->origin && atomic_load(&w->host_done) &&
                 atomic_load(&w->guest_done)) {
        if (helper_live[i]) {
          pthread_join(helpers[i], NULL);
          helper_live[i] = 0;
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
    for (int d = FB_RPC_SLOTS - 1; d >= 0; d--) {
      struct fb_rpc *r = &gw_host_bridge->ctx[gw_host_current->id].rpc[d];
      if (atomic_load_explicit(&r->request, memory_order_acquire) !=
              atomic_load(&r->response) &&
          !atomic_load(&r->active)) {
        fh_service(gw_host_current, r);
      }
    }
  }
}

/* One-time bridge bring-up from the launcher thread: validate the config,
   allocate the zeroed shared bridge (refuses re-init in-process), pin
   BridgeTypes, configure the loader transports and image policy, install
   the legacy adapter when requested, and start the controller. The Guest
   handshake must still pass full_host_ready before any load is allowed. */
int full_host_start(struct shared *s, JNIEnv *env,
                    const struct gw_host_bridge_config *options) {
  if (!s || !env || !options ||
      (options->abi_version != GW_HOST_BRIDGE_CONFIG_V1 &&
       options->abi_version != GW_HOST_BRIDGE_CONFIG_V2) ||
      options->struct_size < GW_HOST_BRIDGE_CONFIG_V1_SIZE ||
      (options->abi_version == GW_HOST_BRIDGE_CONFIG_V2 &&
       options->struct_size < sizeof *options) ||
      (options->flags &
       ~(GW_HOST_LEGACY_JRE17 | GW_HOST_DIRECT_IMAGES |
         GW_HOST_ARRAY_COPY_CACHE | GW_HOST_DISABLE_ARRAY_COPY_CACHE)) ||
      !options->native_directory || !options->image_catalog) {
    return JNI_ERR;
  }

  if (gw_host_bridge) {
    return JNI_ERR; /* Explicitly refuse reinitialization in the same process.
                     */
  }

  const char *dir = options->native_directory;
  legacy_adapter = (options->flags & GW_HOST_LEGACY_JRE17) != 0;
  if (legacy_adapter && !vm_adapter) {
    fprintf(stderr, "JNI adapter requested but jvm-jni is not registered\n");
    return JNI_ERR;
  }
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
  gw_host_bridge->ctx[0].call[0].args[7].j = FB_WAKE_FILTER_HELLO;
#endif

  if ((*env)->GetJavaVM(env, &gw_host_vm) != JNI_OK) {
    return JNI_ERR;
  }
  if (pthread_key_create(&context_key, context_destructor)) {
    fh_throw(env, FB_FAILURE_MEMORY, "Cannot allocate JNI context TLS key");
    return JNI_ERR;
  }

  contexts[0] = (struct host_context){.env = env, .id = 0};
  gw_host_current = &contexts[0];
  if (pthread_setspecific(context_key, gw_host_current)) {
    gw_host_current = NULL;
    fh_throw(env, FB_FAILURE_MEMORY, "Cannot bind initial JNI context");
    return JNI_ERR;
  }
  gw_host_bridge->ctx[0].host_tid = (int)syscall(SYS_gettid);
  atomic_store(&gw_host_bridge->ctx[0].state, FB_RUNNING);
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
  if (legacy_adapter &&
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

/* Handshake gate once the Guest reached phase 2: verify the JNI (and any
   optional wake-filter/profile) acknowledgements, then activate the loader
   so dlopen interception begins. */
int full_host_ready(void) {
  if (jni_accept() != JNI_OK) {
    return JNI_ERR;
  }
  if (!fb_loader_accepted(&gw_host_bridge->ctx[0].call[0])) {
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
        gw_host_bridge->ctx[0].call[0].args[8].j != FB_WAKE_FILTER_ACK) {
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

/* Teardown mirrors full_host_start on the launcher thread: the loader must
   stop cleanly (it refuses while guest images remain live), helpers are
   woken and joined, and the array cache, ID cache, and pinned refs are
   released. The bridge mapping is left in place on purpose — gw_host_bridge
   stays set so a second start is refused. */
int full_host_shutdown(void) {
  if (!gw_host_bridge) {
    return JNI_ERR;
  }
  JNIEnv *env = gw_host_current->env;
  struct gw_loader_error error = {0};
  if (gw_loader_stop(&error) != GW_OK) {
    printf("LOADER_BACKEND teardown_refused=%s\n", error.message);
    return JNI_ERR;
  }

  if (legacy_adapter && vm_adapter->restore(env, &error) != GW_OK) {
    printf("LOADER_BACKEND adapter_restore_failed=%s\n", error.message);
    return JNI_ERR;
  }

  gw_image_policy_finish(&image_policy, env);
  atomic_store(&gw_host_bridge->running, 0);
  fb_event_notify(&gw_host_bridge->host_control);
  for (int i = 0; i < FB_CONTEXTS; i++) {
    fb_event_notify(&gw_host_bridge->ctx[i].host_event);
    fb_event_notify(&gw_host_bridge->ctx[i].guest_event);
  }

  pthread_join(controller, NULL);
  for (int i = 1; i < FB_CONTEXTS; i++) {
    if (helper_live[i]) {
      pthread_join(helpers[i], NULL);
      helper_live[i] = 0;
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
  printf("FULL_HOST_SHUTDOWN helpers_joined=1 id_cache_weak=0\n");
  return JNI_OK;
}
