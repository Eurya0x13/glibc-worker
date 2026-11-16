#define _GNU_SOURCE
#include "runtime_bridge.h"
#include "domain_runtime.h"
#include "dlopen_backend.h"
#include "signal-relay.h"
#include <errno.h>

static int owned_runtime;

int gw_host_runtime_start(JNIEnv *env, const struct gw_host_runtime_config *c) {
  if (!env || !c || c->version != GW_HOST_RUNTIME_CONFIG_V1 ||
      c->size != sizeof *c || c->reserved || !c->loader_path ||
      !*c->loader_path || !c->library_path || !*c->library_path ||
      !c->service_path || !*c->service_path || !c->bridge ||
      (c->backend != GW_EXECUTION_SAME_THREAD &&
       c->backend != GW_EXECUTION_WORKER)) {
    return EINVAL;
  }
  int error = gw_signal_contract_validate(c->signals);
  if (error) {
    return error;
  }
  /* Preparing a VM's existing signal chain belongs to its integration layer.
   * Do not discover or patch ART/HotSpot from this generic JNI component. */
  if (!signal_router_enabled() ||
      c->signals->host_reserved != signal_router_reserved()) {
    return EBUSY;
  }
  error = signal_router_guest_ready();
  if (error) {
    return error;
  }
  if (full_host_state() != GW_HOST_NEW || owned_runtime) {
    return EALREADY;
  }
  if (!signal_host_relay_running()) {
    error = signal_host_relay_start();
    if (error) {
      return error;
    }
  }
#ifdef GW_DOMAIN_NONLOCAL
  error = signal_router_guest_faults();
  if (error) {
    return error;
  }
#endif
  session.shared.magic = MAGIC;
  session.domain = c->backend == GW_EXECUTION_SAME_THREAD;
  session.reserved_signals = c->signals->host_reserved;
  session.note_guest = signal_note_guest_thread;
  session.guest_thread = signal_guest_lifecycle;
#ifdef GW_DOMAIN_NONLOCAL
  session.bind_guest_signal = signal_guest_fault_bind;
#endif
  if (full_host_start(&session.shared, env, c->bridge) != JNI_OK) {
    return EIO;
  }
  owned_runtime = 1;
  error = domain_owner_start(c->loader_path, c->library_path, c->service_path);
  if (!error && full_host_ready() != JNI_OK) {
    error = EPROTO;
  }
  if (error) {
    gw_dlopen_quarantine();
  }
  return error;
}

int gw_host_runtime_shutdown(void) {
  if (!owned_runtime) {
    return EINVAL;
  }
  if (signal_host_retirement_busy() || signal_host_relay_busy()) {
    return EBUSY;
  }
  if (full_host_shutdown() != JNI_OK) {
    return full_host_state() == GW_HOST_QUARANTINED ? EIO : EBUSY;
  }
  int error = owner_command(NULL, JNI_DOMAIN_STOP);
  if (!error) {
    error = signal_host_relay_stop();
  }
  if (error) {
    gw_dlopen_quarantine();
  }
  return error;
}
