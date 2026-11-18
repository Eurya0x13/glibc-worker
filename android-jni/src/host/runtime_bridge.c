#define _GNU_SOURCE
#include "runtime_bridge.h"
#include "domain_runtime.h"
#include "dlopen_backend.h"
#include "signal-relay.h"
#include <errno.h>

static int owned_runtime, owned_relay;
extern int gw_guest_fds_attach(struct gw_guest_fd_state *)
    __attribute__((weak));

int gw_host_runtime_start(JNIEnv *env, const struct gw_host_runtime_config *c) {
  if (!env || !c ||
      (c->version != GW_HOST_RUNTIME_CONFIG_V1 &&
       c->version != GW_HOST_RUNTIME_CONFIG_V2) ||
      c->size != sizeof *c ||
      (c->version == GW_HOST_RUNTIME_CONFIG_V1 && c->reserved) ||
      !c->loader_path || !*c->loader_path || !c->library_path ||
      !*c->library_path || !c->service_path || !*c->service_path ||
      !c->bridge ||
      (c->backend != GW_EXECUTION_SAME_THREAD &&
       c->backend != GW_EXECUTION_WORKER)) {
    return EINVAL;
  }

  int error = gw_signal_contract_validate(c->signals);
  if (error) {
    return error;
  }
  if (c->guest_notification) {
    if (c->guest_notification < (unsigned)SIGRTMIN ||
        c->guest_notification > 62 ||
        (c->signals->host_reserved &
         (UINT64_C(1) << (c->guest_notification - 1)))) {
      return EINVAL;
    }
#ifdef GW_DOMAIN_NONLOCAL
    if (c->backend == GW_EXECUTION_SAME_THREAD &&
        (!signal_router_fault_notifications() || !gw_nonlocal_ready())) {
      return ENOTSUP;
    }
#else
    return ENOTSUP;
#endif
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

  session.shared.magic = MAGIC;
  session.domain = c->backend == GW_EXECUTION_SAME_THREAD;
  session.guest_notification = (int)c->guest_notification;
  session.reserved_signals = c->signals->host_reserved;
  session.note_guest = signal_note_guest_thread;
  session.guest_thread = signal_guest_lifecycle;
#ifdef GW_DOMAIN_NONLOCAL
  session.bind_guest_kernel = signal_guest_kernel_bind;
  session.quarantine_signals = signal_router_quarantine;
  if (session.domain && session.guest_notification) {
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0 || page > 65536 || (65536 % page)) {
      return ENOTSUP;
    }
    session.signal_page_size = (size_t)page;
    session.signal_call = domain_guest_signal_call;
    session.signal_select_host = signal_guest_select_host;
  }
#endif
  /* Claim bridge ownership before creating helpers or changing Guest fault
   * routing. A validation failure leaves NEW and permits a corrected retry;
   * every later failure retains resources under the terminal contract. */
  if (full_host_start(&session.shared, env, c->bridge) != JNI_OK) {
    return EIO;
  }
  owned_runtime = 1;

  if (!signal_host_relay_running()) {
    error = signal_host_relay_start();
    owned_relay = !error;
  }
#ifdef GW_DOMAIN_NONLOCAL
  if (!error && session.domain) {
    error = signal_router_route_host_setxid();
  }
  if (!error) {
    error = signal_router_guest_faults();
  }
  if (!error && session.guest_notification) {
    error = signal_guest_kernel_notify_enable(
        session.guest_notification,
        session.domain ? gw_domain_signal_notification : NULL);
  }
#endif

  if (!error) {
    error =
        domain_owner_start(c->loader_path, c->library_path, c->service_path);
  }
  if (!error && session.guest_fds && gw_guest_fds_attach) {
    error = gw_guest_fds_attach(session.guest_fds);
  }
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
  if (signal_router_error()) {
    gw_dlopen_quarantine();
    return EIO;
  }
  if (signal_host_retirement_busy() || signal_host_relay_busy()) {
    return EBUSY;
  }

  if (full_host_shutdown() != JNI_OK) {
    return full_host_state() == GW_HOST_QUARANTINED ? EIO : EBUSY;
  }

  int error = owner_command(NULL, JNI_DOMAIN_STOP);
  if (!error && owned_relay) {
    error = signal_host_relay_stop();
    if (!error) {
      owned_relay = 0;
    }
  }
  if (error) {
    gw_dlopen_quarantine();
  }

  return error;
}
