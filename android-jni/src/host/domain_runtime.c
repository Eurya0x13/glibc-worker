#define _GNU_SOURCE
#include "domain_runtime.h"
#include "dlopen_backend.h"
#include "nonlocal-mask.h"
#include "signal_channel.h"
#include <time.h>
#include <errno.h>
#ifdef GW_DOMAIN_NONLOCAL
#include "signal-stack.h"
#endif
extern long launch_guest(unsigned long, void *, void *);
extern void domain_start(void);
extern __typeof__(gw_host_futex_wait) gw_host_futex_wait __attribute__((weak));
struct jni_domain_session session;
static pthread_mutex_t owner_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local struct jni_domain_context *current;
static _Thread_local struct signal_slot *current_signal;
_Atomic int domains_created, domains_finished;
_Atomic int fail_next_context, expected_errors;
static _Thread_local int host_tls;

#ifdef GW_DOMAIN_NONLOCAL
int gw_domain_rpc_recoverable(void) {
  return current && current->domain && current->abort_checkpoint;
}

void gw_domain_rpc_enter(struct gw_domain_rpc_scope *scope) {
  REQUIRE(current && current->abort_checkpoint);
  scope->previous = current->host_rpc;
  scope->guest_checkpoint = current->abort_checkpoint;
  scope->previous_guard = current->host_guard;
  current->host_rpc = scope;
  gw_nonlocal_enter(&scope->unwind);
  gw_domain_rpc_guard(scope, 1);
}

void gw_domain_rpc_guard(struct gw_domain_rpc_scope *scope, int guarded) {
  REQUIRE(current->host_rpc == scope);
  scope->unwind.guarded = guarded;
  current->host_guard = guarded ? scope : scope->previous_guard;
}

int gw_domain_rpc_leave(struct gw_domain_rpc_scope *scope, uint64_t host_mask) {
  REQUIRE(current->host_rpc == scope);
  current->host_rpc = scope->previous;
  current->host_guard = scope->previous_guard;
  int resume = current->jump_record &&
               current->jump_target == scope->guest_checkpoint &&
               !gw_nonlocal_pending();
  if (resume) {
    /* Keep delivery masked between retiring the Host RPC and committing the
     * Guest restore. A second transfer cannot abandon half a transaction. */
    current->jump_host_mask = host_mask;
    current->jump_resume = 1;
  }
  gw_nonlocal_leave(&scope->unwind);
  return resume;
}

static void route_guest_jump(void) {
  if (current->jump_signal_target) {
    signal_guest_resume(current->jump_signal_target, session.signal_resume);
  }
  struct gw_domain_rpc_scope *scope = current->host_rpc;
  while (scope && scope->guest_checkpoint != current->jump_target) {
    scope = scope->previous;
  }
  REQUIRE(scope && current->jump_record);
  gw_nonlocal_route(&scope->unwind);
}

/* Called on the restored Host command stack, with asynchronous delivery
 * blocked by the native unwind coordinator. The Guest lands through its own
 * libc on a live dispatcher checkpoint and runs normal bridge cleanup. */
void gw_domain_abort_call(struct fb_call *call, int sequence) {
  struct jni_domain_context *c = current;
  REQUIRE(c && c->domain);
  if (atomic_load_explicit(&call->response, memory_order_acquire) == sequence) {
    return; /* Guest already retired this invocation. */
  }
  if (!c->executing_guest && c->abort_call != call) {
    /* Only this carrier can execute the posted command. It has not entered
     * Guest yet, so cancel the publication before Guest can observe it. An
     * outer Guest checkpoint is not the checkpoint of this request. */
    atomic_store_explicit(&call->response, sequence, memory_order_release);
    return;
  }
  REQUIRE(c->abort_call == call && c->abort_checkpoint && c->abort_entry);
  if (!c->executing_guest && c->guest_rpc_owner == c->abort_checkpoint) {
    /* Its Host RPC has already retired while walking the Host suffix. Let
     * the suspended Guest exchange retire its own stack scopes and metrics
     * before aborting this invocation through the owning libc. */
    c->abort_resume = 1;
    gw_signal_stack_get(&c->exchange.host_altstack);
    c->exchange.swap(&c->exchange.host, &c->exchange.guest);
    gw_signal_stack_set(&c->exchange.host_altstack);
    return;
  }
  if (c->executing_guest) {
    uint64_t logical = session.guest_notification
                           ? session.guest_virtual_signals
                           : session.reserved_signals;
    c->guest_mask = (c->exchange.interrupted_guest_mask & ~logical) |
                    (c->guest_mask & logical);
    c->guest_altstack = c->exchange.interrupted_guest_altstack;
  }
  struct domain abort = c->exchange.guest;
  abort.sp = c->abort_stack + 65536;
  abort.pc = (uintptr_t)domain_start;
#if defined(__aarch64__)
  abort.saved[12] = c->exchange.interrupted_control[2];
  abort.saved[21] = c->exchange.interrupted_control[0];
  abort.saved[22] = c->exchange.interrupted_control[1];
  abort.saved[0] = (uintptr_t)c;
  abort.saved[1] = (uintptr_t)c->abort_entry;
#else
  abort.saved[6] = c->exchange.interrupted_control[0];
  abort.saved[7] = c->exchange.interrupted_control[1];
  abort.saved[2] = (uintptr_t)c;
  abort.saved[3] = (uintptr_t)c->abort_entry;
#endif
  /* A Host AUTODISARM handler may leave its stack disabled or install another
   * stack before jumping. Keep that actual Host state across Guest abort. */
  gw_signal_stack_get(&c->exchange.host_altstack);
  c->exchange.swap(&c->exchange.host, &abort);
  gw_signal_stack_set(&c->exchange.host_altstack);
  c->executing_guest = 0;
}
#endif

int adoption_signal_check(struct signal_slot *slot) {
  return slot->exchange->host_tls == (uintptr_t)&host_tls &&
         slot->exchange->host_tid == syscall(SYS_gettid);
}

int gw_domain_host_active(void) {
  return current && current->domain;
}

static int reject_damaged_router(struct fb_call *call, int sequence) {
  if (!signal_router_error()) {
    return 0;
  }

  /* This is an ordinary Host boundary, never the asynchronous signal route.
   * Quarantine may lock the loader. Do not resume a partially active Guest or
   * reclaim its image/resources after an uncertain signal-state transition. */
  gw_dlopen_quarantine();
  call->error = GW_E_QUARANTINED;
  call->result = (jvalue){0};
  snprintf(call->diagnostic, sizeof call->diagnostic,
           "Host signal router quarantined; discard process");
  atomic_store_explicit(&call->response, sequence, memory_order_release);
  return 1;
}

#ifdef GW_DOMAIN_NONLOCAL
static void release_signal_stack(void *argument) {
  struct jni_domain_signal *activation = argument;
  if (syscall(SYS_munmap, activation->mapping, activation->mapping_size)) {
    signal_router_quarantine();
  }
  activation->mapping = NULL;
}

static int run_parked_signal(struct jni_domain_context *c, void *frame) {
  const size_t size = 65536, page = session.signal_page_size;
  struct gw_signal_suspension suspended;
  int kernel = signal_guest_suspended(current_signal, &suspended);
  if (!kernel) {
    if (!c->parked_guest) {
      return ESRCH;
    }
    suspended =
        (struct gw_signal_suspension){.frame = c->parked_guest,
                                      .state = c->exchange.guest,
                                      .alternate = c->guest_altstack,
                                      .stack_floor = c->exchange.guest.sp};
  }

  struct jni_domain_signal activation = {.carrier = c,
                                         .frame = suspended.frame,
                                         .stack_floor = suspended.stack_floor,
                                         .kernel_frame = kernel,
                                         .mapping_size = size + 2 * page};
  activation.mapping =
      (void *)syscall(SYS_mmap, NULL, activation.mapping_size, PROT_NONE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (activation.mapping == MAP_FAILED) {
    return errno;
  }
  void *stack = (char *)activation.mapping + page;
  if (syscall(SYS_mprotect, stack, size, PROT_READ | PROT_WRITE)) {
    int error = errno;
    release_signal_stack(&activation);
    return error;
  }

  struct gw_signal_execution execution;
  activation.execution = &execution;
  const struct gw_signal_execution_config config = {
      .stack = stack,
      .stack_size = size,
      .abort_stack = (void *)c->abort_stack,
      .abort_stack_size = 65536,
      .alternate = suspended.alternate,
      .swap = c->exchange.swap,
      .invoke = session.signal_invoke,
      .finish = session.signal_finish,
      .release_host = release_signal_stack,
      .argument = &activation,
      .carrier_mask = &activation.carrier_mask};
  int error = gw_signal_execution_from_saved(
      &execution, &config, &suspended.state, &session.signal_entries);
  if (!error) {
    error = signal_guest_execute(&execution, frame);
  }
  if (activation.mapping) {
    release_signal_stack(&activation);
  }
  return error;
}

void gw_domain_signal_notification(int number, siginfo_t *packet, void *frame) {
  struct jni_domain_context *c = current;
  if (!c || !current_signal || !current_signal->guest_async ||
      number != session.guest_notification || packet->si_code != SI_QUEUE ||
      packet->si_pid != getpid() ||
      (uintptr_t)packet->si_value.sival_ptr != session.signal_cookie) {
    signal_guest_notification_discard(frame);
    return;
  }
  int error = run_parked_signal(c, frame);
  if (error == ECANCELED && c->jump_record) {
    route_guest_jump();
  }
  if (error) {
    signal_router_quarantine();
  }
}
#endif

int gw_domain_command(struct host_context *host, struct fb_call *call,
                      int sequence) {
  (void)host;
  if (reject_damaged_router(call, sequence)) {
    return 1;
  }
  if (!gw_domain_host_active()) {
    return 0;
  }
  struct exchange *e = &current->exchange;
  while (atomic_load_explicit(&call->response, memory_order_acquire) !=
         sequence) {
#ifdef GW_DOMAIN_NONLOCAL
    uint64_t host_mask = gw_nonlocal_mask_block();
    if (current->jump_resume) {
      REQUIRE(current->jump_record &&
              current->abort_checkpoint == current->jump_target);
      host_mask = current->jump_host_mask;
      current->jump_resume = 0;
    }
    e->host_execution_mask = host_mask;
    e->guest_execution_mask = signal_carrier_mask(current_signal, host_mask, 1);
    e->host_mask_valid = 1;
    gw_signal_stack_get(&e->host_altstack);
    current->executing_guest = 1;
#endif
    e->swap(&e->host, &e->guest);
#ifdef GW_DOMAIN_NONLOCAL
    current->executing_guest = 0;
    gw_signal_stack_set(&e->host_altstack);
    signal_host_prune_notifications();
    if (current->jump_record) {
      route_guest_jump();
    }
    /* Guest RPCs may update guest_mask before yielding. The Host mask
     * belongs to this particular suspended Host frame, including when the
     * callback re-enters another native command. */
    host_mask = signal_host_physical_mask(signal_host_logical_mask(host_mask));
    gw_nonlocal_mask_restore(host_mask);
    if (reject_damaged_router(call, sequence)) {
      return 1;
    }
#endif
    full_host_poll();
  }
  return 1;
}

static void *stack_alloc(size_t size) {
  size_t page = (size_t)sysconf(_SC_PAGESIZE);
  char *p = mmap(NULL, size + 2 * page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS,
                 -1, 0);
  if (p == MAP_FAILED) {
    return NULL;
  }
  if (mprotect(p + page, size, PROT_READ | PROT_WRITE)) {
    munmap(p, size + 2 * page);
    return NULL;
  }
  return p + page;
}

static int wait_for_owner(_Atomic int *word, int wanted) {
  struct timespec deadline;
  if (clock_gettime(CLOCK_MONOTONIC, &deadline)) {
    return errno;
  }
  deadline.tv_sec += 15;

  int observed;
  while ((observed = atomic_load_explicit(word, memory_order_acquire)) !=
         wanted) {
    int result =
        gw_host_futex_wait
            ? gw_host_futex_wait(word, (uint32_t)observed, &deadline)
            : syscall(SYS_futex, word, FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG,
                      observed, &deadline, NULL, FUTEX_BITSET_MATCH_ANY);
    if (result < 0 && errno != EINTR && errno != EAGAIN) {
      return atomic_load_explicit(word, memory_order_acquire) == wanted ? 0
                                                                        : errno;
    }
  }

  return 0;
}

int owner_command(struct jni_domain_context *context, int operation) {
  pthread_mutex_lock(&owner_lock);
  int seq = gw_rpc_next_sequence(atomic_load(&session.request));
  session.context = context;
  session.operation = operation;
  atomic_store_explicit(&session.request, seq, memory_order_release);
  fb_event_notify(&fb_context_at(gw_host_bridge, 0)->guest_event);
  int error = 0;
  if (operation == JNI_DOMAIN_STOP || operation == JNI_DOMAIN_CHECK_STOP) {
    error = wait_for_owner(&session.response, seq);
  } else {
    int observed;
    while ((observed = atomic_load_explicit(&session.response,
                                            memory_order_acquire)) != seq) {
      wait_word(&session.response, observed);
    }
  }

  if (!error) {
    error = session.error;
    if (!error && operation == JNI_DOMAIN_STOP &&
        session.owner_stopped_mask != GW_DOMAIN_STOP_MASK) {
      error = EPROTO;
    }
  }
  pthread_mutex_unlock(&owner_lock);
  return error;
}

int domain_owner_start(const char *loader, const char *libraries,
                       const char *service) {
  int lease_error = signal_router_lease_guest_nptl();
  if (lease_error) {
    return lease_error;
  }

  session.handshake = (struct jni_domain_handshake){
      .magic = GW_DOMAIN_TRANSPORT_MAGIC,
      .version = GW_DOMAIN_TRANSPORT_VERSION,
      .size = sizeof session,
      .context_size = sizeof(struct jni_domain_context),
      .exchange_size = sizeof(struct exchange),
      .siginfo_size = sizeof(siginfo_t),
      .ucontext_size = sizeof(ucontext_t),
      .sigmask_offset = offsetof(ucontext_t, uc_sigmask),
      .mcontext_offset = offsetof(ucontext_t, uc_mcontext),
      .required = GW_DOMAIN_CAP_ADOPTION | GW_DOMAIN_CAP_STOP_QUIESCENT};
#ifdef GW_DOMAIN_NONLOCAL
  session.handshake.required |= GW_DOMAIN_CAP_NATIVE_UNWIND;
  if (session.guest_notification) {
    session.handshake.required |= GW_DOMAIN_CAP_WORKER_ASYNC;
    if (session.domain) {
      session.handshake.required |= GW_DOMAIN_CAP_ADOPTED_ASYNC;
    }
  }
#endif
  struct gw_bootstrap_image image;
  int error = gw_bootstrap_map_checked(loader, &image);
  if (error) {
    return error;
  }
  int service_fd;
  error =
      gw_bootstrap_open_note(service, GW_DOMAIN_NOTE_OWNER, GW_DOMAIN_NOTE_TYPE,
                             GW_DOMAIN_TRANSPORT_VERSION, &service_fd);
  if (error) {
    munmap(image.base, image.size);
    return error;
  }

  const size_t size = 2 * 1024 * 1024;
  void *stack = stack_alloc(size);
  if (!stack) {
    error = errno;
    close(service_fd);
    munmap(image.base, image.size);
    return error;
  }
  char pointer[32], opened[48];
  snprintf(pointer, sizeof pointer, "%lx", (unsigned long)(uintptr_t)&session);
  snprintf(opened, sizeof opened, "/proc/self/fd/%d", service_fd);
  char *arguments[] = {(char *)loader, "--library-path", (char *)libraries,
                       opened, pointer};
  void *sp;
  error = gw_bootstrap_stack_checked(image, stack, size, arguments, 5, &sp);
  long tid = 0;
  if (!error) {
    uint64_t mask = gw_nonlocal_mask_block();
    session.owner_start_mask = mask;
    tid = launch_guest(CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
                           CLONE_THREAD | CLONE_SYSVSEM,
                       sp, image.entry);
    gw_nonlocal_mask_restore(mask);
    if (tid < 0) {
      error = (int)-tid;
    }
  }
  if (error) {
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    close(service_fd);
    munmap((char *)stack - page, size + 2 * page);
    munmap(image.base, image.size);
    return error;
  }
  /* Once cloned, the stack and loader are process-pinned, including errors.
   * A published ready flag does not prove that the kernel has exited a thread.
   * Keep the validated service fd on timeout: the loader may yet open its
   * /proc path, which must continue to identify the original file. */
  error = wait_for_owner(&session.ready, 1);
  if (error) {
    return error;
  }
  close(service_fd);
  if (session.error) {
    return session.error;
  }
  return (session.handshake.accepted & session.handshake.required) ==
                 session.handshake.required
             ? 0
             : EPROTO;
}

void start_owner(char **arguments) {
  REQUIRE(!domain_owner_start(arguments[1], arguments[2], arguments[3]));
}

/* Called only after fh_context has reserved its stable production slot. */
int gw_domain_context_start(struct host_context *host) {
  struct fb_context *w = fb_context_at(gw_host_bridge, host->id);
  int injected = atomic_exchange(&fail_next_context, 0);
  if (injected) {
    atomic_fetch_add(&expected_errors, 1);
  }
  if (injected && !session.domain) {
    w->attach_result = JNI_ENOMEM;
    atomic_store_explicit(&w->state, FB_ATTACH_FAILED, memory_order_release);
    return 1;
  }
  if (!session.domain) {
    return 0;
  }
  struct jni_domain_context *c = calloc(1, sizeof *c);
  if (!c) {
    w->attach_result = JNI_ENOMEM;
    atomic_store_explicit(&w->state, FB_ATTACH_FAILED, memory_order_release);
    return 1;
  }
  REQUIRE(!current);
  current = c;
  c->id = host->id;
  c->domain = 1;
  c->fail_allocate = injected;
  struct exchange *e = &c->exchange;
  e->swap = domain_swap;
  e->host_tid = w->host_tid;
  e->host_tls = (uintptr_t)&host_tls;
  e->stack_low = (uintptr_t)stack_alloc(2 * 1024 * 1024);
  if (!e->stack_low) {
    goto fail;
  }
  e->stack_high = e->stack_low + 2 * 1024 * 1024;
#ifdef GW_DOMAIN_NONLOCAL
  c->abort_stack = (uintptr_t)stack_alloc(65536);
  c->guest_altstack.ss_flags = SS_DISABLE;
  if (!c->abort_stack) {
    goto fail;
  }
#endif
  current_signal = signal_acquire(e);
  if (!current_signal || owner_command(c, JNI_DOMAIN_ALLOC)) {
    goto fail;
  }
  e->guest.pc = (uintptr_t)domain_start;
#if defined(__aarch64__)
  e->guest.saved[0] = (uintptr_t)c;
  e->guest.saved[1] = (uintptr_t)session.entry;
#else
  e->guest.saved[2] = (uintptr_t)c;
  e->guest.saved[3] = (uintptr_t)session.entry;
  __asm__ volatile("stmxcsr %0" : "=m"(e->guest.saved[6]));
  __asm__ volatile("fnstcw %0" : "=m"(e->guest.saved[7]));
#endif
#ifdef GW_DOMAIN_NONLOCAL
  uint64_t host_mask = gw_nonlocal_mask_block();
  c->guest_mask = host_mask;
  e->host_execution_mask = host_mask;
  e->guest_execution_mask = signal_carrier_mask(current_signal, host_mask, 1);
  e->host_mask_valid = 1;
  gw_signal_stack_get(&e->host_altstack);
#endif
  e->swap(&e->host, &e->guest);
#ifdef GW_DOMAIN_NONLOCAL
  gw_signal_stack_set(&e->host_altstack);
  if (session.guest_notification) {
    REQUIRE(!signal_guest_carrier_attach(current_signal));
    e->guest_execution_mask = signal_carrier_mask(current_signal, host_mask, 1);
    host_mask = signal_host_physical_mask(signal_host_logical_mask(host_mask));
  }
  gw_nonlocal_mask_restore(host_mask);
#endif
  REQUIRE(atomic_load(&c->ready));
  atomic_fetch_add(&domains_created, 1);
  atomic_store_explicit(&w->state, FB_RUNNING, memory_order_release);
  return 1;
fail:
  if (current_signal) {
    signal_release(current_signal);
  }
  current_signal = NULL;
  if (c->abort_stack) {
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    munmap((void *)(c->abort_stack - page), 65536 + 2 * page);
  }
  if (e->stack_low) {
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    munmap((void *)(e->stack_low - page), 2 * 1024 * 1024 + 2 * page);
  }
  free(c);
  current = NULL;
  w->attach_result = JNI_ENOMEM;
  atomic_store_explicit(&w->state, FB_ATTACH_FAILED, memory_order_release);
  return 1;
}

int gw_domain_context_stop(struct host_context *host, int thread_exiting) {
  if (!current) {
    return 0;
  }
  struct jni_domain_context *c = current;
  REQUIRE(c->id == host->id);
  if (c->live_signals || c->abort_checkpoint || c->host_rpc || c->jump_record ||
      c->host_guard || c->jump_resume || c->guest_rpc_owner ||
      c->abort_resume) {
    return -EBUSY;
  }
  int error = signal_prepare_release(current_signal, thread_exiting);
  if (error) {
    return -error;
  }
  atomic_store(&c->stop, 1);
#ifdef GW_DOMAIN_NONLOCAL
  uint64_t host_mask = gw_nonlocal_mask_block();
  gw_signal_stack_get(&c->exchange.host_altstack);
#endif
  c->exchange.swap(&c->exchange.host, &c->exchange.guest);
#ifdef GW_DOMAIN_NONLOCAL
  gw_signal_stack_set(&c->exchange.host_altstack);
  if (session.guest_notification) {
    REQUIRE(!signal_guest_carrier_detach(current_signal));
    host_mask = signal_host_physical_mask(signal_host_logical_mask(host_mask));
  }
  gw_nonlocal_mask_restore(host_mask);
#endif
  REQUIRE(c->exchange.state == DOMAIN_DONE);
  REQUIRE(!owner_command(c, JNI_DOMAIN_FREE));
  signal_release(current_signal);
  current_signal = NULL;
  size_t page = (size_t)sysconf(_SC_PAGESIZE);
  if (c->abort_stack) {
    REQUIRE(!munmap((void *)(c->abort_stack - page), 65536 + 2 * page));
  }
  REQUIRE(!munmap((void *)(c->exchange.stack_low - page),
                  2 * 1024 * 1024 + 2 * page));
  free(c);
  current = NULL;
  atomic_fetch_add(&domains_finished, 1);
  atomic_store_explicit(&fb_context_at(gw_host_bridge, host->id)->state,
                        FB_JOINED, memory_order_release);
  return 1;
}
