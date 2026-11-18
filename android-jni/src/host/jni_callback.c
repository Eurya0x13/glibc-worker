#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include "host_internal.h"
#ifdef GW_DOMAIN_NONLOCAL
#include "domain_runtime.h"
#include "domain_unwind.h"
#endif

typedef void (*discard_result)(struct host_context *, jobject, const void *);

struct rpc_result {
  discard_result discard;
  jobject owner, token;
  const void *value;
  jboolean *copy_out;
  jboolean copy;
  int temporary;
};

struct callback_state {
  uint64_t module;
  int marshal_error, emergency;
};

struct jni_callback {
  struct host_context *context;
  struct fb_rpc *request;
#ifdef GW_DOMAIN_NONLOCAL
  struct gw_domain_rpc_scope *scope;
  struct rpc_result result;
#endif
};

#ifdef GW_DOMAIN_NONLOCAL_TEST
static void observe_result(struct jni_callback *call, int point,
                           uintptr_t value, uintptr_t token) {
  if (gw_domain_nonlocal_test_result) {
    gw_domain_nonlocal_test_result(point, call->request->api, value, token);
  }
}
#else
#define observe_result(call, point, value, token) ((void)0)
#endif

/* A producer returns from JNI before opening the native window. Keep its
 * inverse live until commit, including while preparing the reply fields. */
static void own_result(struct jni_callback *call, struct rpc_result owned) {
#ifdef GW_DOMAIN_NONLOCAL
  if (call->scope) {
    REQUIRE(!call->result.discard);
    call->result = owned;
    uint64_t mask = gw_nonlocal_mask_block();
    gw_domain_rpc_guard(call->scope, 0);
    gw_nonlocal_mask_restore(mask);
    return;
  }
#endif
  if (owned.temporary) {
    owned.discard(call->context, owned.owner, owned.value);
  }
  if (owned.copy_out) {
    *owned.copy_out = owned.copy;
  }
}

#ifdef GW_DOMAIN_NONLOCAL
static void settle_result(struct jni_callback *call, int abandoned) {
  struct rpc_result result = call->result;
  if (!result.discard) {
    return;
  }

  call->result = (struct rpc_result){0};
  /* These JNI release calls are permitted with a pending exception.
   * https://docs.oracle.com/en/java/javase/17/docs/specs/jni/design.html#exception-handling
   */
#ifdef GW_DOMAIN_NONLOCAL_TEST
  uintptr_t value = (uintptr_t)result.value, token = (uintptr_t)result.token;
#endif
  if (abandoned || result.temporary) {
    result.discard(call->context, result.owner, result.value);
  }
  if (abandoned) {
    if (result.token) {
      fh_drop_token(result.token);
    }
    call->request->result = (jvalue){0};
    observe_result(call, GW_UNWIND_RPC_RESULT_DISCARDED, value, token);
  } else if (result.copy_out) {
    *result.copy_out = result.copy;
  }
}
#endif

static void discard_local(struct host_context *c, jobject extra,
                          const void *reference) {
  JNIEnv *env = c->env;
  if (extra) {
    (*env)->DeleteLocalRef(env, extra);
  }
  if (reference) {
    (*env)->DeleteLocalRef(env, (jobject)reference);
  }
}

static void discard_global(struct host_context *c, jobject unused,
                           const void *reference) {
  (void)unused;
  (*c->env)->DeleteGlobalRef(c->env, (jobject)reference);
}

static void discard_weak(struct host_context *c, jobject unused,
                         const void *reference) {
  (void)unused;
  (*c->env)->DeleteWeakGlobalRef(c->env, (jweak)reference);
}

static void reference_result(struct jni_callback *call, jobject value,
                             int kind) {
  if (!value) {
    return;
  }

  discard_result discard = kind == H_LOCAL    ? discard_local
                           : kind == H_GLOBAL ? discard_global
                                              : discard_weak;
  jobject token = fh_add_ref(call->context, value, kind);
  if (!token) {
    discard(call->context, NULL, value);
    call->request->error = 2;
    return;
  }

  own_result(call, (struct rpc_result){
                       .discard = discard, .token = token, .value = value});
  call->request->result.l = token;
}

static void id_result(struct jni_callback *call, void *value, int kind,
                      const char *signature, jobject member, jclass owner) {
  call->request->result.l =
      fh_add_id(call->context, value, kind, signature, owner);
  if (member || owner) {
    own_result(call, (struct rpc_result){.discard = discard_local,
                                         .owner = member,
                                         .value = owner,
                                         .temporary = 1});
  }
}

static void discard_frame(struct host_context *c, jobject unused,
                          const void *unused_value) {
  (void)unused;
  (void)unused_value;
  (*c->env)->PopLocalFrame(c->env, NULL);
  fh_drop_locals(c, c->local_depth);
  --c->local_depth;
}

static void push_frame_result(struct jni_callback *call, jint status) {
  if (status == JNI_OK) {
    ++call->context->local_depth;
    own_result(call, (struct rpc_result){.discard = discard_frame});
  }
  call->request->result.i = status;
}

static void discard_monitor(struct host_context *c, jobject object,
                            const void *unused) {
  (void)unused;
  if ((*c->env)->MonitorExit(c->env, object) != JNI_OK) {
    /* A failed inverse leaves the monitor's ownership uncertain. */
    _exit(125);
  }
}

static void monitor_enter_result(struct jni_callback *call, jobject object,
                                 jint status) {
  if (status == JNI_OK) {
    own_result(
        call, (struct rpc_result){.discard = discard_monitor, .owner = object});
  }
  call->request->result.i = status;
}

static void discard_chars(struct host_context *c, jobject string,
                          const void *chars) {
  (*c->env)->ReleaseStringChars(c->env, string, chars);
}

static void discard_utf_chars(struct host_context *c, jobject string,
                              const void *chars) {
  (*c->env)->ReleaseStringUTFChars(c->env, string, chars);
}

static void discard_string_critical(struct host_context *c, jobject string,
                                    const void *chars) {
  (*c->env)->ReleaseStringCritical(c->env, string, chars);
  --c->critical;
}

static void discard_array_critical(struct host_context *c, jobject array,
                                   const void *elements) {
  (*c->env)->ReleasePrimitiveArrayCritical(c->env, array, (void *)elements,
                                           JNI_ABORT);
  --c->critical;
}

#define DISCARD_ELEMENTS(type)                                                 \
  static void discard_##type##_elements(struct host_context *c, jobject array, \
                                        const void *elements) {                \
    fh_release_##type##_elements(c->env, (j##type##Array)array,                \
                                 (j##type *)elements, JNI_ABORT);              \
  }

DISCARD_ELEMENTS(boolean)
DISCARD_ELEMENTS(byte)
DISCARD_ELEMENTS(char)
DISCARD_ELEMENTS(short)
DISCARD_ELEMENTS(int)
DISCARD_ELEMENTS(long)
DISCARD_ELEMENTS(float)
DISCARD_ELEMENTS(double)
#undef DISCARD_ELEMENTS

static void pointer_result(struct jni_callback *call, jobject owner,
                           const void *value, discard_result discard,
                           jboolean copy, int critical) {
  if (!value) {
    return;
  }

  call->context->critical += critical;
  own_result(call,
             (struct rpc_result){.discard = discard,
                                 .owner = owner,
                                 .value = value,
                                 .copy_out = (jboolean *)call->request->a[1].l,
                                 .copy = copy});
  call->request->result.l = (jobject)value;
}

static void dispatch(struct jni_callback *call) {
  struct host_context *c = call->context;
  struct fb_rpc *r = call->request;
  JNIEnv *env = c->env;

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
    return;
  }

#include "generated/host-jni.inc"
}

static struct callback_state prepare_callback(struct jni_callback *call) {
  struct host_context *c = call->context;
  struct fb_rpc *r = call->request;
  struct callback_state saved = {.module = c->module,
                                 .marshal_error = c->marshal_error,
                                 .emergency = c->emergency};
  c->module = r->module;
  c->marshal_error = 0;
  if (r == &fb_context_at(gw_host_bridge, c->id)->emergency) {
    c->emergency = 1;
  }
  if (r->api == 14) {
    printf(
        "JNI_EXPECTED_FATAL stage=host-forward index=14 name=FatalError "
        "calls=%u\n",
        atomic_load(&gw_host_bridge->hits[14]));
  }
  return saved;
}

static void restore_callback(struct jni_callback *call,
                             struct callback_state saved) {
  struct host_context *c = call->context;
  struct fb_rpc *r = call->request;
  if (c->marshal_error && !r->error) {
    r->error = c->marshal_error;
  }
  c->marshal_error = saved.marshal_error;
  c->emergency = saved.emergency;
  if (r->error) {
    fh_throw(c->env, FB_FAILURE_STATE, "JNI bridge request failed");
    atomic_fetch_add(&gw_host_bridge->errors, 1);
  }
  c->module = saved.module;
}

/* Serve one claimed JNI request. The landing, resource state and cleanup must
 * survive through publication of the response to the still-parked Guest RPC. */
static void serve_callback(struct jni_callback *call, int sequence, int recover,
                           uint64_t admission_mask) {
  struct host_context *c = call->context;
  struct fb_rpc *r = call->request;
  int saved_errno = errno;
#if GW_RPC_PROFILE
  unsigned handoff_depth = (unsigned)r->depth;
  struct gw_handoff_row *handoff = gw_rpc_peer_handoff((unsigned)c->id);
  if (handoff_depth < GW_HANDOFF_DEPTH) {
    gw_handoff_dispatch(handoff, handoff_depth);
  }
  if (r->api >= 0) {
    gw_rpc_served((unsigned)r->api);
  }
#endif
  GW_RPC_SCOPE(glue, GW_RPC_HOST_JNI_GLUE, -1);

  struct callback_state saved = prepare_callback(call);

#ifdef GW_DOMAIN_NONLOCAL
  struct gw_domain_rpc_scope scope;
  int abandoned = 0;
  if (recover) {
    call->scope = &scope;
    if (!sigsetjmp(scope.unwind.landing, 0)) {
      gw_domain_rpc_enter(&scope);
      /* GetVersion is served from the cached Host value in pure C. */
      if (r->api == 0) {
        gw_domain_rpc_guard(&scope, 0);
      }
      gw_nonlocal_mask_restore(admission_mask);
      if (r->api == 0) {
        GW_UNWIND_POINT(GW_UNWIND_RPC_READY);
      }
      dispatch(call);
#ifdef GW_DOMAIN_NONLOCAL_TEST
      if (call->result.discard) {
        observe_result(call, GW_UNWIND_RPC_RESULT_OWNED,
                       (uintptr_t)call->result.value,
                       (uintptr_t)call->result.token);
      }
#endif
    } else {
      abandoned = 1;
    }
  } else
#else
  (void)recover;
  (void)admission_mask;
#endif
  {
    dispatch(call);
  }
#ifdef GW_DOMAIN_NONLOCAL
  uint64_t cleanup_mask = recover ? gw_nonlocal_mask_block() : 0;
  if (recover) {
    gw_domain_rpc_guard(&scope, 1);
    settle_result(call, abandoned);
  }
#endif

  restore_callback(call, saved);
  GW_RPC_SCOPE_END(glue);
  errno = saved_errno;
  atomic_store(&r->active, 0);

#if GW_RPC_PROFILE
  if (handoff_depth < GW_HANDOFF_DEPTH) {
    gw_handoff_complete(handoff, handoff_depth);
  }
#endif
  atomic_store_explicit(&r->response, sequence, memory_order_release);
#ifdef GW_DOMAIN_NONLOCAL
  if (recover && !gw_domain_rpc_leave(&scope, admission_mask)) {
    gw_nonlocal_mask_restore(cleanup_mask);
  }
#endif
#ifdef GW_DOMAIN_JNI
  extern int gw_domain_host_active(void);
  if (gw_domain_host_active()) {
    return;
  }
#endif
  fb_event_notify(&fb_context_at(gw_host_bridge, c->id)->guest_event);
}

void fh_service(struct host_context *c, struct fb_rpc *r) {
  int sequence = atomic_load_explicit(&r->request, memory_order_acquire);
  if (sequence == atomic_load(&r->response)) {
    return;
  }

  int recover = 0;
  uint64_t admission_mask = 0;
#ifdef GW_DOMAIN_NONLOCAL
  recover = gw_domain_rpc_recoverable();
  if (recover) {
    admission_mask = gw_nonlocal_mask_block();
  }
#endif
  if (atomic_exchange(&r->active, 1)) {
#ifdef GW_DOMAIN_NONLOCAL
    if (recover) {
      gw_nonlocal_mask_restore(admission_mask);
    }
#endif
    return;
  }

  struct jni_callback call = {.context = c, .request = r};
  serve_callback(&call, sequence, recover, admission_mask);
}
