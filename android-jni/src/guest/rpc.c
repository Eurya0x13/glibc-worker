/* The final RPC slot remains usable when native nesting is exhausted. */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "guest_internal.h"
#include "generated/guest-policy.inc"

int fg_valid(JNIEnv *env) {
  return G && gid >= 0 && gid < FB_CONTEXTS && env == &fg_env;
}

static jvalue exchange(int api, jvalue *args, int slot) {
  jvalue zero = {0};
  GW_RPC_SCOPE(boundary, GW_RPC_STAGE_COUNT, -1);
#if GW_RPC_PROFILE
  struct gw_rpc_token metrics = gw_rpc_begin();
#endif

  GW_RPC_SCOPE(pack, GW_RPC_GUEST_RPC_PACK, -1);
  if (api >= 0) {
    atomic_fetch_add(&G->hits[api], 1);
  }
  struct fb_rpc *r = &G->ctx[gid].rpc[slot];
  memcpy(r->a, args, sizeof r->a);
  r->module = gmodule;
  r->api = api;
  r->error = 0;
  r->result = zero;
  int seq = gw_rpc_next_sequence(
      atomic_load_explicit(&r->request, memory_order_relaxed));
#if GW_RPC_PROFILE
  if (slot < FB_DEPTH) {
    gw_handoff_begin(&gw_rpc_current->handoff, (unsigned)slot);
  }
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
  if (slot < FB_DEPTH) {
    gw_handoff_finish(
        &gw_rpc_current->handoff, (unsigned)slot,
        (struct gw_handoff_record){.module = gmodule,
                                   .api = (unsigned)api,
                                   .context = (unsigned)gid,
                                   .depth = (unsigned)slot,
                                   .host_tid = G->ctx[gid].host_tid,
                                   .guest_tid = G->ctx[gid].guest_tid,
                                   .request = (unsigned)seq});
  }
#endif

#if GW_RPC_PROFILE
  gw_rpc_end(metrics);
#endif
  GW_RPC_SCOPE_END(boundary);
  return r->error ? fg_failure_result(api) : r->result;
}

jvalue fg_rpc(JNIEnv *env, int api, jvalue *args) {
  if (!fg_valid(env)) {
    errno = EINVAL;
    if (G) {
      atomic_fetch_add(&G->errors, 1);
    }
    return fg_failure_result(
        api); /* No JNIEnv exists on an unattached thread. */
  }
  if (gdepth < 0 || gdepth > FB_DEPTH) {
    errno = EOVERFLOW;
    atomic_fetch_add(&G->errors, 1);
    return fg_failure_result(api);
  }
  if (gdepth == FB_DEPTH && !fg_emergency_api(api)) {
    jvalue failure[8] = {{.i = FB_FAILURE_DEPTH}};
    exchange(FB_RPC_REPORT_FAILURE, failure, FB_DEPTH);
    return fg_failure_result(api);
  }
  return exchange(api, args, gdepth);
}
