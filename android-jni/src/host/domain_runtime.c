#define _GNU_SOURCE
#include "domain_runtime.h"
#include "dlopen_backend.h"
#include <time.h>
#include <errno.h>
#ifdef GW_DOMAIN_NONLOCAL
#include "nonlocal-mask.h"
#include "signal-stack.h"
#endif
extern long launch_guest(unsigned long, void *, void *);
extern void domain_start(void);
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
  current->host_rpc = scope;
  gw_nonlocal_enter(&scope->unwind);
}

void gw_domain_rpc_leave(struct gw_domain_rpc_scope *scope) {
  REQUIRE(current->host_rpc == scope);
  current->host_rpc = scope->previous;
  if (current->jump_target == scope->guest_checkpoint) {
    current->jump_resume = 1;
  }
  gw_nonlocal_leave(&scope->unwind);
}

static void route_guest_jump(void) {
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
    c->guest_mask =
        (c->exchange.interrupted_guest_mask & ~session.reserved_signals) |
        (c->guest_mask & session.reserved_signals);
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
    e->host_execution_mask = host_mask;
    e->guest_execution_mask = signal_carrier_mask(current_signal, host_mask, 1);
    e->host_mask_valid = 1;
    gw_signal_stack_get(&e->host_altstack);
    current->executing_guest = 1;
    if (current->jump_resume) {
      REQUIRE(current->jump_record &&
              current->abort_checkpoint == current->jump_target);
      current->jump_resume = 0;
    }
#endif
    e->swap(&e->host, &e->guest);
#ifdef GW_DOMAIN_NONLOCAL
    current->executing_guest = 0;
    gw_signal_stack_set(&e->host_altstack);
    signal_host_prune_notifications();
    /* Guest RPCs may update guest_mask before yielding. The Host mask
     * belongs to this particular suspended Host frame, including when the
     * callback re-enters another native command. */
    host_mask = signal_host_physical_mask(signal_host_logical_mask(host_mask));
    gw_nonlocal_mask_restore(host_mask);
    if (reject_damaged_router(call, sequence)) {
      return 1;
    }
    if (current->jump_record) {
      route_guest_jump();
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

int owner_command(struct jni_domain_context *context, int operation) {
  pthread_mutex_lock(&owner_lock);
  int seq = gw_rpc_next_sequence(atomic_load(&session.request));
  session.context = context;
  session.operation = operation;
  atomic_store_explicit(&session.request, seq, memory_order_release);
  fb_event_notify(&fb_context_at(gw_host_bridge, 0)->guest_event);
  int observed;
  while ((observed = atomic_load_explicit(&session.response,
                                          memory_order_acquire)) != seq) {
    wait_word(&session.response, observed);
  }
  int error = session.error;
  if (!error && operation == JNI_DOMAIN_STOP &&
      session.owner_stopped_mask != GW_DOMAIN_STOP_MASK) {
    error = EPROTO;
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
#endif
  char pointer[32];
  snprintf(pointer, sizeof pointer, "%lx", (unsigned long)(uintptr_t)&session);
  char *arguments[] = {(char *)loader, "--library-path", (char *)libraries,
                       (char *)service, pointer};
  struct gw_bootstrap_image image;
  int error = gw_bootstrap_map_checked(loader, &image);
  if (error) {
    return error;
  }
  const size_t size = 2 * 1024 * 1024;
  void *stack = stack_alloc(size);
  if (!stack) {
    error = errno;
    munmap(image.base, image.size);
    return error;
  }
  void *sp;
  error = gw_bootstrap_stack_checked(image, stack, size, arguments, 5, &sp);
  long tid = 0;
  if (!error) {
    tid = launch_guest(CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
                           CLONE_THREAD | CLONE_SYSVSEM,
                       sp, image.entry);
    if (tid < 0) {
      error = (int)-tid;
    }
  }
  if (error) {
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    munmap((char *)stack - page, size + 2 * page);
    munmap(image.base, image.size);
    return error;
  }
  /* Once cloned, the stack and loader are process-pinned, including errors.
   * A published ready flag does not prove that the kernel has exited a thread.
   * Never reclaim its stack based on an application-level completion flag. */
  struct timespec beginning, now;
  if (clock_gettime(CLOCK_MONOTONIC, &beginning)) {
    return errno;
  }
  while (!atomic_load_explicit(&session.ready, memory_order_acquire)) {
    struct timespec timeout = {.tv_nsec = 100000000};
    syscall(SYS_futex, &session.ready, FUTEX_WAIT_PRIVATE, 0, &timeout, NULL,
            0);
    if (clock_gettime(CLOCK_MONOTONIC, &now)) {
      return errno;
    }
    if (now.tv_sec - beginning.tv_sec >= 15) {
      return ETIMEDOUT;
    }
  }
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
  if (c->abort_checkpoint || c->host_rpc || c->jump_record || c->jump_resume ||
      c->guest_rpc_owner || c->abort_resume) {
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
