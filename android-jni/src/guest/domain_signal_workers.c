#define _GNU_SOURCE
#include "domain_signal_workers.h"
#include "domain_jump.h"
#include "domain_signals.h"
#include "domain_transport.h"
#include "nonlocal-mask.h"
#include "signal-consuming.h"
#include "signal-stack.h"
#include "signal-user-copy.h"
#include "thread-create.h"
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <sys/resource.h>

static struct gw_signal_targets targets;
static struct gw_signal_target *owner;
static __thread struct gw_signal_target *current;
static __thread struct jni_domain_context *carrier;
static __thread uint64_t logical;
static uint64_t physical, virtual_mask;
static int notification;
static const int binding_active = 1;
static int (*mark_error)(void);
static int (*bind_mask)(uint64_t *, const uint64_t *, const int *, uint64_t);

static uint64_t bit(int number) {
  return UINT64_C(1) << (number - 1);
}

void gw_guest_workers_wake_error(int error) {
  if (error && mark_error) {
    mark_error(); /* The Host callback is explicitly TLS-free. */
  }
}

static void check_publication(struct gw_signal_publication result) {
  gw_guest_workers_wake_error(result.status ? result.status
                                            : result.wake_error);
}

static void mask_changed(uint64_t mask) {
  if (current) {
    check_publication(gw_signal_target_mask(current, mask));
  }
}

static int notify(void *unused, struct gw_signal_target *target) {
  (void)unused;
  pid_t pid = getpid();
  siginfo_t information = {.si_signo = notification, .si_code = SI_QUEUE};
  /* GNU's public names designate different arms of the siginfo_t union.
   * Assign the common PID/UID prefix and queued value after initialization. */
  information.si_pid = pid;
  information.si_uid = getuid();
  information.si_value.sival_ptr = &targets;
  return syscall(SYS_rt_tgsigqueueinfo, pid, target->tid, notification,
                 &information)
             ? errno
             : 0;
}

static int send_signal(void *thread, int tid, const siginfo_t *information,
                       int *handled) {
  *handled = 1;
  struct gw_signal_publication result = gw_signal_actions_send(
      &gw_guest_actions, (uintptr_t)thread, tid, information);
  gw_guest_workers_wake_error(result.wake_error);
  return result.status;
}

static int pending(sigset_t *set, int *handled) {
  *handled = current != NULL;
  return current ? gw_guest_sigpending(current, set) : 0;
}

static int consuming(const sigset_t *set, siginfo_t *information,
                     const struct timespec *timeout, gw_libc_wait_event event,
                     int *handled) {
  *handled = current != NULL;
  return current
             ? gw_guest_sigtimedwait(current, set, information, timeout, event)
             : 0;
}

static int prepare_child(void *key, uint64_t inherited, uint64_t kernel,
                         void **cookie) {
  (void)kernel;
  struct gw_signal_target *target;
  int error = gw_signal_target_prepare(&targets, (uintptr_t)key, inherited,
                                       NULL, &target);
  if (error) {
    return error;
  }
  error =
      gw_signal_target_retain(target); /* Child hold, even if parent wins. */
  if (error) {
    gw_signal_target_close(target);
    gw_signal_target_retire(target);
    return error;
  }
  *cookie = target;
  return 0;
}

static void complete_child(void *cookie, int tid, int error) {
  struct gw_signal_target *target = cookie;
  if (error) {
    /* NPTL guarantees that failure after clone skips application entry. */
    gw_signal_target_close(target);
    gw_guest_workers_wake_error(gw_signal_target_retire(target));
  } else {
    int activated = gw_signal_target_activate(target, tid);
    /* A detached child can already have finished and closed its target. */
    if (activated != ESRCH) {
      gw_guest_workers_wake_error(activated);
    }
    gw_guest_workers_wake_error(gw_signal_targets_error(&targets));
  }
  gw_guest_workers_wake_error(gw_signal_target_retire(target));
}

static void enter_child(void *cookie) {
  struct gw_signal_target *target = cookie;
  current = target;
  logical = target->receiver.blocked;
  if (bind_mask(&logical, &physical, &binding_active, virtual_mask) ||
      gw_signal_target_activate(target, (int)syscall(SYS_gettid))) {
    _exit(126);
  }
  gw_guest_workers_wake_error(gw_signal_targets_error(&targets));
}

static void leave_child(void *cookie) {
  struct gw_signal_target *target = cookie;
  if (target != current) {
    _exit(126);
  }
  /* NPTL has finished TLS destructors and blocks catchable application
   * signals for the rest of this thread's life. No frame can resume here. */
  current = NULL;
  gw_signal_target_close(target);
  gw_guest_workers_wake_error(gw_signal_target_retire(target));
}

static int install_hooks(void) {
  int (*thread_hook)(unsigned, const struct gw_libc_thread_create_hook *) =
      dlsym(RTLD_DEFAULT, "__gw_domain_thread_create_hook");
  int (*mask_hook)(unsigned, void (*)(uint64_t)) =
      dlsym(RTLD_DEFAULT, "__gw_domain_mask_hook");
  int (*send_hook)(unsigned, int (*)(void *, int, const siginfo_t *, int *)) =
      dlsym(RTLD_DEFAULT, "__gw_domain_signal_send_hook");
  int (*consume_hook)(unsigned, const struct gw_libc_signal_consume_hook *) =
      dlsym(RTLD_DEFAULT, "__gw_domain_signal_consume_hook");
  bind_mask = dlsym(RTLD_DEFAULT, "__gw_domain_mask_bind");
  if (!thread_hook || !mask_hook || !send_hook || !consume_hook || !bind_mask) {
    return ELIBBAD;
  }

  const struct gw_libc_thread_create_hook admission = {
      .size = sizeof admission,
      .prepare = prepare_child,
      .complete = complete_child,
      .enter = enter_child,
      .leave = leave_child};
  const struct gw_libc_signal_consume_hook wait = {
      .size = sizeof wait, .pending = pending, .wait = consuming};
  int error = bind_mask(&logical, &physical, &binding_active, virtual_mask);
  if (error || (error = mask_hook(1, mask_changed)) ||
      (error = thread_hook(1, &admission)) ||
      (error = send_hook(1, send_signal)) || (error = consume_hook(1, &wait))) {
    return error;
  }
  return 0;
}

int gw_guest_workers_start(int helper, uint64_t reserved,
                           int (*quarantine)(void)) {
  if (!helper || !quarantine) {
    return EINVAL;
  }
  struct rlimit limit;
  if (getrlimit(RLIMIT_SIGPENDING, &limit)) {
    return errno;
  }
  int error = gw_signal_user_copy_prepare();
  if (error) {
    return error;
  }

  static const uint64_t nptl = UINT64_C(3) << 62;
  const uint64_t faults = bit(SIGSEGV) | bit(SIGBUS) | bit(SIGILL) |
                          bit(SIGFPE) | bit(SIGTRAP) | bit(SIGSYS);
  const uint64_t job_control =
      bit(SIGTSTP) | bit(SIGTTIN) | bit(SIGTTOU) | bit(SIGCONT);
  notification = helper;
  mark_error = quarantine;
  virtual_mask = ~(nptl | bit(SIGKILL) | bit(SIGSTOP));
  logical = gw_nonlocal_mask_block();
  physical = logical & ~(bit(helper) | faults);
  reserved |= nptl | bit(helper) | job_control;
  gw_signal_targets_init(&targets, reserved, limit.rlim_cur, notify, NULL);
  gw_guest_signals_init(&targets, reserved & ~faults);

  error = gw_signal_targets_enable_waits(&targets);
  if (error ||
      (error = gw_signal_target_open(&targets, (uintptr_t)pthread_self(),
                                     (int)syscall(SYS_gettid), logical, NULL,
                                     &owner)) ||
      (error = gw_signal_actions_representative(&gw_guest_actions, owner))) {
    return error;
  }
  current = owner;
  return install_hooks();
}

void gw_guest_workers_unmask(void) {
  gw_nonlocal_mask_restore(physical);
}

static struct gw_guest_signal_state signal_state(void) {
  struct gw_guest_signal_state state = {.logical = &logical,
                                        .carrier = physical,
                                        .virtual_mask = virtual_mask,
                                        .target = current};
  if (carrier) {
    state.logical = &carrier->guest_mask;
    state.carrier = carrier->exchange.guest_execution_mask;
  }
  return state;
}

uint64_t gw_guest_virtual_mask(void) {
  return virtual_mask;
}

uintptr_t gw_guest_signal_cookie(void) {
  return (uintptr_t)&targets;
}

int gw_guest_signal_adopt(struct jni_domain_context *context) {
  int error =
      bind_mask(&context->guest_mask, &context->exchange.guest_execution_mask,
                &context->guest_native_active, virtual_mask);
  if (error) {
    return error;
  }
  carrier = context;
  return gw_signal_target_open(&targets, (uintptr_t)pthread_self(),
                               (int)syscall(SYS_gettid), context->guest_mask,
                               NULL, &current);
}

int gw_guest_signal_retire(void) {
  struct gw_signal_target *target = current;
  if (!target || !carrier) {
    return EINVAL;
  }
  current = NULL;
  carrier = NULL;
  gw_signal_target_close(target);
  return gw_signal_target_release(target);
}

int gw_guest_workers_fault(int number, void *information, void *frame) {
  return current ? gw_guest_signal_dispatch(number, information, frame,
                                            signal_state())
                 : EINVAL;
}

static void deliver(void *frame, struct gw_guest_signal_state state) {
  struct gw_signal_delivery delivery;
  int selected;
  while ((selected = gw_signal_actions_deliver(&gw_guest_actions, current,
                                               &delivery)) > 0) {
    gw_guest_signal_deliver(&delivery, frame, state);
  }
  gw_guest_workers_wake_error(-selected);
}

void gw_guest_workers_notify(void *packet, void *frame) {
  siginfo_t *notification_packet = packet;
  if (current && notification_packet->si_code == SI_QUEUE &&
      notification_packet->si_pid == getpid() &&
      notification_packet->si_value.sival_ptr == &targets) {
    deliver(frame, signal_state());
  }
}

int gw_guest_signal_parked(void *argument) {
  struct jni_domain_signal *activation = argument;
  if (carrier != activation->carrier || !activation->frame || !current) {
    return ESRCH;
  }
  if (gw_signal_target_pin(current)) {
    return ESRCH;
  }
  activation->pinned = 1;
  activation->initial_mask = carrier->guest_mask;
  activation->previous_carrier_mask = carrier->exchange.guest_execution_mask;
  carrier->exchange.guest_execution_mask = activation->carrier_mask;
  activation->previous_active = carrier->guest_native_active;
  carrier->guest_native_active = 1;
  activation->previous = carrier->guest_signal;
  carrier->guest_signal = activation;

  ucontext_t *parked = activation->frame;
  ucontext_t frame = *parked;
#if defined(__x86_64__)
  if (frame.uc_mcontext.fpregs == &parked->__fpregs_mem) {
    frame.uc_mcontext.fpregs = &frame.__fpregs_mem;
  }
#endif
  struct gw_guest_signal_state state = signal_state();
  state.stack_floor = activation->stack_floor;
  deliver(&frame, state);
#if defined(__x86_64__)
  if (frame.uc_mcontext.fpregs == &frame.__fpregs_mem) {
    frame.uc_mcontext.fpregs = &parked->__fpregs_mem;
  }
#endif
  if (activation->kernel_frame) {
    /* The older Guest sigreturn still owns the physical Host reservations.
     * Only its Guest register/stack snapshot and nonvirtual bits may change. */
    uint64_t mask;
    memcpy(&mask, &parked->uc_sigmask, sizeof mask);
    mask = (mask & state.virtual_mask) | (*state.logical & ~state.virtual_mask);
    memcpy(&frame.uc_sigmask, &mask, sizeof mask);
    *parked = frame;
    return 0;
  }
  carrier->guest_altstack = frame.uc_stack;
  carrier->guest_altstack.ss_flags &= ~SS_ONSTACK;
  if (memcmp(&parked->uc_mcontext, &frame.uc_mcontext, sizeof frame.uc_mcontext)
#if defined(__x86_64__)
      || memcmp(&parked->__fpregs_mem, &frame.__fpregs_mem,
                sizeof frame.__fpregs_mem)
#endif
  ) {
    *parked = frame;
    carrier->resume_parked = 1;
  }
  return 0;
}

void gw_guest_signal_finish_parked(void *argument, enum gw_signal_exit reason) {
  struct jni_domain_signal *activation = argument;
  if (!activation->pinned) {
    return;
  }
  if (carrier != activation->carrier || !current ||
      carrier->guest_signal != activation) {
    _exit(126);
  }
  if (reason == GW_SIGNAL_HOST_JUMP && !carrier->jump_record) {
    carrier->guest_mask = activation->initial_mask;
    check_publication(gw_signal_target_mask(current, carrier->guest_mask));
  } else if (reason == GW_SIGNAL_GUEST_JUMP) {
    gw_signal_stack_get(&carrier->guest_altstack);
    check_publication(gw_signal_target_mask(current, carrier->guest_mask));
  }

  gw_domain_jump_signal_leave(activation);
  carrier->guest_signal = activation->previous;
  carrier->guest_native_active = activation->previous_active;
  carrier->exchange.guest_execution_mask = activation->previous_carrier_mask;
  gw_guest_workers_wake_error(gw_signal_target_unpin(current));
}

int gw_guest_workers_stop(void) {
  /* Caller joined bridge workers and froze native module producers. GNU
   * application threads still present invalidate this proof. Keep mappings. */
  gw_nonlocal_mask_block();
  if (current != owner || !gw_signal_targets_only(&targets, owner)) {
    return EBUSY;
  }
  gw_signal_actions_close(&gw_guest_actions);
  current = NULL;
  gw_signal_targets_close(&targets);
  int error = gw_signal_target_release(owner);
  return error ? error : gw_signal_targets_destroy(&targets);
}
