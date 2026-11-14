#include <stdio.h>
#include <string.h>
#include "host_internal.h"
#include "dlopen_backend.h"

/* Post one fb_call at the current nesting depth and block for its reply.
   Re-entrant by design: a native call may re-enter Java and issue deeper
   commands, each on its own stable call slot, growing on demand. While blocked
   the thread pumps its own inbound JNI callbacks via full_host_poll, so the
   Guest is never stalled waiting on this thread. No failure escapes as a C
   return code: every failed path records c->command_error with c->diagnostic
   text (plus the typed Java error where the caller must observe one), leaving
   the rollback decision to the caller. */
jvalue fh_command(struct host_context *c, int op, uintptr_t fn, const char *sig,
                  jvalue *args) {
  jvalue zero = {0};

  c->command_error = 0;
  c->diagnostic[0] = 0;
  if (gw_dlopen_is_quarantined()) {
    c->command_error = GW_E_QUARANTINED;
    snprintf(c->diagnostic, sizeof c->diagnostic,
             "Guest bridge quarantined; discard process");
    fh_throw(c->env, FB_FAILURE_STATE, c->diagnostic);
    return zero;
  }

  size_t signature_length = sig ? strnlen(sig, FB_SIG) : 0;
  if (signature_length == FB_SIG) {
    snprintf(c->diagnostic, sizeof c->diagnostic,
             "Command descriptor exceeds bridge capacity");
    c->command_error = GW_E_ARGUMENT;
    fh_throw(c->env, FB_FAILURE_ARGUMENT, c->diagnostic);
    atomic_fetch_add(&gw_host_bridge->errors, 1);
    return zero;
  }

  struct fb_context *w = fb_context_at(gw_host_bridge, c->id);
  if (c->emergency || c->depth < 0 || c->depth >= INT_MAX - 1 || !w ||
      !fb_ensure_frame(w, c->depth + 1)) {
    c->command_error = GW_E_CAPACITY;
    snprintf(c->diagnostic, sizeof c->diagnostic,
             "Cannot grow Host bridge frames");
    fh_throw(c->env, FB_FAILURE_MEMORY, c->diagnostic);
    atomic_fetch_add(&gw_host_bridge->errors, 1);
    return zero;
  }

  GW_RPC_SCOPE(boundary, GW_RPC_STAGE_COUNT, -1);
#if GW_RPC_PROFILE
  struct gw_rpc_token metrics =
      op == FB_NATIVE ? gw_rpc_begin() : (struct gw_rpc_token){0};
#endif

  GW_RPC_SCOPE(pack, GW_RPC_HOST_COMMAND_PACK, -1);
  struct fb_call *r = fb_call_at(w, c->depth++);
  r->op = op;
  r->module = c->module;
  r->function = fn;
  r->error = 0;
  r->result = zero;
  r->diagnostic[0] = 0;
  r->sig[0] = 0;
  if (sig) {
    memcpy(r->sig, sig, signature_length + 1);
  }
  if (args) {
    memcpy(r->args, args, sizeof r->args);
  } else {
    memset(r->args, 0, sizeof r->args);
  }

  int seq = gw_rpc_next_sequence(atomic_load(&r->request));
#if GW_RPC_PROFILE
  unsigned handoff_depth = (unsigned)(c->depth - 1);
  if (op == FB_NATIVE && handoff_depth < GW_HANDOFF_DEPTH) {
    gw_handoff_begin(&gw_rpc_current->handoff, handoff_depth);
  }
#endif
  atomic_store_explicit(&r->request, seq, memory_order_release);
  struct fb_event *event = &fb_context_at(gw_host_bridge, c->id)->host_event;
  GW_RPC_SCOPE_END(pack);
  fb_event_notify(&fb_context_at(gw_host_bridge, c->id)->guest_event);

  for (;;) {
    uint32_t observed = fb_event_sample(event);
    full_host_poll();
    if (atomic_load_explicit(&r->response, memory_order_acquire) == seq) {
      break;
    }
#if GW_RPC_READY_POLL
    fb_event_wait_active_response(event, observed, &r->response, seq);
#else
    fb_event_wait_active(event, observed);
#endif
  }

#if GW_RPC_PROFILE
  if (op == FB_NATIVE && handoff_depth < GW_HANDOFF_DEPTH) {
    gw_handoff_finish(
        &gw_rpc_current->handoff, handoff_depth,
        (struct gw_handoff_record){
            .function = fn,
            .module = c->module,
            .api = UINT64_MAX,
            .context = (unsigned)c->id,
            .depth = handoff_depth,
            .host_tid = fb_context_at(gw_host_bridge, c->id)->host_tid,
            .guest_tid = fb_context_at(gw_host_bridge, c->id)->guest_tid,
            .request = (unsigned)seq});
  }
#endif

  GW_RPC_SCOPE(finish, GW_RPC_HOST_COMMAND_FINISH, -1);
  c->command_error = r->error;
  snprintf(c->diagnostic, sizeof c->diagnostic, "%s", r->diagnostic);
  c->depth--;
  if (r->error && op == FB_NATIVE) {
    fh_throw(
        c->env, FB_FAILURE_STATE,
        c->diagnostic[0] ? c->diagnostic : "Guest native invocation failed");
  }
  GW_RPC_SCOPE_END(finish);
#if GW_RPC_PROFILE
  gw_rpc_end(metrics);
#endif
  GW_RPC_SCOPE_END(boundary);
  return r->result;
}
