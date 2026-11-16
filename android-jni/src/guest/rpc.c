/* Independent emergency RPC remains usable when dynamic frame growth fails. */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "guest_internal.h"
#include "generated/guest-policy.inc"
#ifdef GW_DOMAIN_NONLOCAL
#include "nonlocal-mask.h"
#include "domain_jump.h"
extern int gw_domain_guest_active(void);
extern uint64_t gw_domain_rpc_return_mask(uint64_t);
#endif

/* A JNIEnv passes only when it is this thread's own Guest facade: the bridge
   is mapped, the thread holds a live context slot, and the pointer equals
   this thread's fg_env singleton. A real VM JNIEnv or another thread's
   facade never qualifies. */
int fg_valid(JNIEnv *env) {
  return G && gid >= 0 && gid < fb_context_capacity(G) && env == &fg_env;
}

/* Post one request on the chosen rpc slot and block for its reply, pumping
   inbound Host work while waiting. Publishing the slot index into rpc_top is
   what lets the owning Host context's full_host_poll route a nested
   Guest->Host callback to this blocked thread; the saved top is restored so
   a later exchange at the same depth reuses the slot. slot < 0 selects the
   reserved emergency slot, which never touches rpc_top. A guest-side error
   yields the canned failure value for this api. */
static jvalue exchange(int api, jvalue *args, int slot) {
  jvalue zero = {0};
  uint64_t execution_mask = 0;
  (void)execution_mask;
#ifdef GW_DOMAIN_NONLOCAL
  int domain = gw_domain_guest_active();
  if (domain) {
    execution_mask = gw_nonlocal_mask_block();
  }
#endif
  GW_RPC_SCOPE(boundary, GW_RPC_STAGE_COUNT, -1);
#if GW_RPC_PROFILE
  struct gw_rpc_token metrics = gw_rpc_begin();
#endif

  GW_RPC_SCOPE(pack, GW_RPC_GUEST_RPC_PACK, -1);
  if (api >= 0) {
    atomic_fetch_add(&G->hits[api], 1);
  }
  struct fb_context *context = fb_context_at(G, gid);
  struct fb_rpc *r = slot < 0 ? &context->emergency : fb_rpc_at(context, slot);
  int previous_top =
      atomic_load_explicit(&context->rpc_top, memory_order_relaxed);
  r->depth = slot;
  memcpy(r->a, args, sizeof r->a);
  r->module = gmodule;
  r->api = api;
  r->error = 0;
  r->result = zero;
  int seq = gw_rpc_next_sequence(
      atomic_load_explicit(&r->request, memory_order_relaxed));
#if GW_RPC_PROFILE
  if (slot >= 0 && (unsigned)slot < GW_HANDOFF_DEPTH) {
    gw_handoff_begin(&gw_rpc_current->handoff, (unsigned)slot);
  }
#endif
  if (slot >= 0) {
    atomic_store_explicit(&context->rpc_top, slot, memory_order_release);
  }
  atomic_store_explicit(&r->request, seq, memory_order_release);
  GW_RPC_SCOPE_END(pack);

#ifdef GW_DOMAIN_JNI
  extern int gw_domain_rpc(struct fb_rpc *, int, uint64_t);
  if (!gw_domain_rpc(r, seq, execution_mask)) {
#endif
    fb_event_notify(&fb_context_at(G, gid)->host_event);
    for (;;) {
      uint32_t observed = fb_event_sample(&fb_context_at(G, gid)->guest_event);
      fg_pump();
      if (atomic_load_explicit(&r->response, memory_order_acquire) == seq) {
        break;
      }
#if GW_RPC_READY_POLL
      fb_event_wait_active_response(&fb_context_at(G, gid)->guest_event,
                                    observed, &r->response, seq);
#else
    fb_event_wait_active(&fb_context_at(G, gid)->guest_event, observed);
#endif
    }
#ifdef GW_DOMAIN_JNI
  }
#endif
#if GW_RPC_PROFILE
  if (slot >= 0 && (unsigned)slot < GW_HANDOFF_DEPTH) {
    gw_handoff_finish(&gw_rpc_current->handoff, (unsigned)slot,
                      (struct gw_handoff_record){
                          .module = gmodule,
                          .api = (unsigned)api,
                          .context = (unsigned)gid,
                          .depth = (unsigned)slot,
                          .host_tid = fb_context_at(G, gid)->host_tid,
                          .guest_tid = fb_context_at(G, gid)->guest_tid,
                          .request = (unsigned)seq});
  }
#endif

#if GW_RPC_PROFILE
  gw_rpc_end(metrics);
#endif
  GW_RPC_SCOPE_END(boundary);
  if (slot >= 0) {
    atomic_store_explicit(&context->rpc_top, previous_top,
                          memory_order_release);
  }
#ifdef GW_DOMAIN_NONLOCAL
  if (domain) {
    /* A suspended RPC returns through its normal ownership/metrics cleanup
     * before a pending transfer abandons its caller. Do not jump directly
     * from a disjoint scratch stack over these bridge-owned frames. */
    gw_domain_rpc_finish();
    gw_nonlocal_mask_restore(gw_domain_rpc_return_mask(execution_mask));
  }
#endif
  return r->error ? fg_failure_result(api) : r->result;
}

/* Entry for the generated JNI table: validate the calling thread, then run
   one exchange at the current nesting depth. When the frame cannot grow,
   emergency APIs still go out on the reserved slot while ordinary ones report
   FB_FAILURE_MEMORY to the Host and return the canned failure value. */
jvalue fg_rpc(JNIEnv *env, int api, jvalue *args) {
  if (!fg_valid(env)) {
    errno = EINVAL;
    if (G) {
      atomic_fetch_add(&G->errors, 1);
    }
    return fg_failure_result(
        api); /* No JNIEnv exists on an unattached thread. */
  }
  if (gdepth < 0) {
    errno = EOVERFLOW;
    atomic_fetch_add(&G->errors, 1);
    return fg_failure_result(api);
  }

  if (!fb_ensure_frame(fb_context_at(G, gid), gdepth)) {
    if (fg_emergency_api(api)) {
      return exchange(api, args, -1);
    }
    jvalue failure[8] = {{.i = FB_FAILURE_MEMORY}};
    exchange(FB_RPC_REPORT_FAILURE, failure, -1);
    return fg_failure_result(api);
  }

  return exchange(api, args, gdepth);
}
