#define _GNU_SOURCE
#include "domain_transport.h"
#include "guest_internal.h"
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <setjmp.h>
#include <sys/prctl.h>
#ifdef GW_DOMAIN_NONLOCAL
#include "domain_unwind.h"
#include "domain_signals.h"
#include "domain_signal_workers.h"
#include "domain_jump.h"
#include "nonlocal-mask.h"
#include "signal-stack.h"
#endif

static struct jni_domain_session *session;
static __thread struct jni_domain_context *current;
static int (*install_thread_hook)(void (*)(int));
static int (*allocate_domain)(void *, size_t, void **, void **, void **);
static int (*bind_domain)(void *);
static int (*run_domain)(void (*)(void *), void *);
static int (*finish_domain)(void);
static int (*free_domain)(void *);
static int (*reserve_signals)(uint64_t);
#ifdef GW_DOMAIN_NONLOCAL
static int (*bind_mask)(uint64_t *, const uint64_t *, const int *, uint64_t);
static int (*install_sigaction_hook)(int (*)(int, const void *, void *, int *));
static int (*install_jump_hook)(void (*)(void *, int, int));
static int (*install_context_hook)(unsigned, gw_libc_context_hook);

static uint64_t virtual_signals(void) {
  return session->guest_notification ? gw_guest_virtual_mask()
                                     : session->reserved_signals;
}

static uint64_t logical_mask(uint64_t physical) {
  uint64_t virtual = virtual_signals();
  return (physical & ~virtual) | (current->guest_mask & virtual);
}

static uint64_t physical_mask(uint64_t logical) {
  uint64_t virtual = virtual_signals();
  return (logical & ~virtual) |
         (current->exchange.guest_execution_mask & virtual);
}
#endif

#ifdef GW_DOMAIN_NONLOCAL
uint64_t gw_domain_execution_mask(void) {
  return physical_mask(current->guest_mask);
}

uint64_t gw_domain_reserved_signals(void) {
  return virtual_signals();
}

void gw_domain_signal_select(struct jni_domain_signal *source,
                             struct jni_domain_signal *target) {
  struct gw_signal_execution *from = source->execution, *to = target->execution;
  struct domain_signal_call call = {.entry = session->signal_select_host,
                                    .tp = from->host.tp,
                                    .info = from,
                                    .context = to};
#if defined(__aarch64__)
  call.x18 = from->host.saved[12];
  call.control[0] = from->host.saved[21];
  call.control[1] = from->host.saved[22];
#else
  call.control[0] = from->host.saved[6];
  call.control[1] = from->host.saved[7];
#endif
  if (session->signal_call(&call) || gw_guest_signal_select(from, to)) {
    _exit(125);
  }
  current->guest_signal = target;
  current->exchange.guest_execution_mask = target->carrier_mask;
}

uint64_t gw_domain_rpc_return_mask(uint64_t original) {
  return current->guest_native_active ? physical_mask(current->guest_mask)
                                      : original;
}

int gw_domain_native_enter(void) {
  int previous = current->guest_native_active;
  current->guest_native_active = 1;
  return previous;
}

void gw_domain_native_leave(int previous, int completed) {
  uint64_t mask = gw_nonlocal_mask_block();
  gw_signal_stack_get(&current->guest_altstack);
  if (completed) {
    current->guest_mask = logical_mask(mask);
  }
  current->guest_native_active = previous;
}

static void abort_native(void *argument) {
  struct jni_domain_context *c = argument;
  gw_signal_stack_set(&c->guest_altstack);
  struct gw_domain_native_checkpoint *frame = c->abort_checkpoint;
  sigjmp_buf *checkpoint = frame->buffer;
  siglongjmp(*checkpoint, 1);
}

static int guest_fault(int number, void *info, void *context) {
  if (session->guest_notification) {
    return gw_guest_workers_fault(number, info, context);
  }
  if (!current) {
    /* Genuine GNU pthreads use their native mask, stack, errno and TCB. They
     * have no suspended Host frame and therefore no Host logical mask. */
    uint64_t logical;
    memcpy(&logical, &((ucontext_t *)context)->uc_sigmask, sizeof logical);
    return gw_guest_signal_dispatch(
        number, info, context,
        (struct gw_guest_signal_state){.logical = &logical});
  }
  struct gw_guest_signal_state state = {
      .logical = &current->guest_mask,
      .carrier = current->exchange.guest_execution_mask,
      .virtual_mask = session->reserved_signals};
  return gw_guest_signal_dispatch(number, info, context, state);
}

static void guest_kernel_entry(int number, void *info, void *context) {
  if (number == session->guest_notification) {
    gw_guest_workers_notify(info, context);
    return;
  }
  if (guest_fault(number, info, context)) {
    _exit(126);
  }
}

static void suspend_for_host(struct jni_domain_context *c) {
  ucontext_t parked = {0};
  volatile sig_atomic_t resumed = 0;
  c->parked_guest = &parked;
  if (getcontext(&parked)) {
    _exit(125);
  }
  if (resumed) {
    return;
  }

  resumed = 1;
  parked.uc_stack = c->guest_altstack;
  c->exchange.swap(&c->exchange.guest, &c->exchange.host);
  c->parked_guest = NULL;
  if (c->resume_parked) {
    c->resume_parked = 0;
    /* Resume this owning GNU context with delivery masked. The logical
     * signal-mask edits have already been committed to the Guest recipient. */
    sigfillset(&parked.uc_sigmask);
    gw_domain_restore_context(&parked);
  }
}
#endif

int gw_domain_guest_active(void) {
  return current && current->domain;
}

static void yield_host(void) {
  struct exchange *e = &current->exchange;
#ifdef GW_DOMAIN_NONLOCAL
  gw_nonlocal_mask_block();
  gw_signal_stack_get(&current->guest_altstack);
  if (session->guest_notification) {
    suspend_for_host(current);
  } else
#endif
  {
    e->swap(&e->guest, &e->host);
  }
#ifdef GW_DOMAIN_NONLOCAL
  gw_signal_stack_set(&current->guest_altstack);
#endif
}

int gw_domain_rpc(struct fb_rpc *rpc, int sequence, uint64_t execution_mask) {
#ifndef GW_DOMAIN_NONLOCAL
  (void)execution_mask;
#endif
  if (!gw_domain_guest_active()) {
    return 0;
  }
#ifdef GW_DOMAIN_NONLOCAL
  int active = current->guest_native_active;
  void *previous_rpc_owner = current->guest_rpc_owner;
  current->guest_rpc_owner = current->abort_checkpoint;
  if (active) {
    current->guest_mask = logical_mask(execution_mask);
  }
#endif
  while (atomic_load_explicit(&rpc->response, memory_order_acquire) !=
         sequence) {
#ifdef GW_DOMAIN_NONLOCAL
    current->guest_native_active = 0;
#endif
    yield_host();
    fg_pump(); /* Java may have re-entered a deeper native call. */
#ifdef GW_DOMAIN_NONLOCAL
    current->guest_native_active = active;
#endif
  }
#ifdef GW_DOMAIN_NONLOCAL
  current->guest_rpc_owner = previous_rpc_owner;
#endif
  return 1;
}

static void serve(void *argument) {
  struct jni_domain_context *c = argument;
  current = c;
#ifdef GW_DOMAIN_NONLOCAL
  c->abort_entry = abort_native;
  gw_domain_jump_bind(c);
  c->exchange.guest_signal = guest_fault;
  gw_signal_stack_set(&c->guest_altstack);
  int error =
      session->guest_notification
          ? gw_guest_signal_adopt(c)
          : bind_mask(&c->guest_mask, &c->exchange.guest_execution_mask,
                      &c->guest_native_active, session->reserved_signals);
  if (error) {
    _exit(126);
  }
#endif
  gid = c->id;
  gdepth = 0;
  gmodule = 0;
  struct fb_context *w = fb_context_at(G, gid);
  w->guest_tid = (int)syscall(SYS_gettid);
  atomic_store_explicit(&c->ready, 1, memory_order_release);
  wake_word(&c->ready);
  yield_host();
  while (!atomic_load(&c->stop)) {
    fg_pump();
    yield_host();
  }
#ifdef GW_DOMAIN_NONLOCAL
  if (session->guest_notification && gw_guest_signal_retire()) {
    _exit(126);
  }
  gw_domain_jump_finish();
#endif
  gid = -1;
  current = NULL;
}

static void adopted_entry(void *argument) {
  struct jni_domain_context *c = argument;
  if (bind_domain(c->token) || run_domain(serve, c) || finish_domain()) {
    _exit(121);
  }
  c->exchange.state = DOMAIN_DONE;
  c->exchange.swap(&c->exchange.guest, &c->exchange.host);
  _exit(122);
}

static int resolve(void) {
#define API(variable, name)                          \
  do {                                               \
    *(void **)&variable = dlsym(RTLD_DEFAULT, name); \
    if (!variable) {                                 \
      return ELIBBAD;                                \
    }                                                \
  } while (0)
  unsigned long (*capabilities)(void);
  API(capabilities, "__gw_domain_capabilities");
  if (capabilities() != GW_DOMAIN_LIBC_CAPABILITIES) {
    return ELIBBAD;
  }
  API(install_thread_hook, "__gw_domain_thread_hook");
  API(allocate_domain, "__gw_domain_alloc");
  API(bind_domain, "__gw_domain_bind");
  API(run_domain, "__gw_domain_run");
  API(finish_domain, "__gw_domain_finish");
  API(free_domain, "__gw_domain_free");
  API(reserve_signals, "__gw_domain_reserve_signals");
#ifdef GW_DOMAIN_NONLOCAL
  API(bind_mask, "__gw_domain_mask_bind");
  API(install_sigaction_hook, "__gw_domain_sigaction_hook");
  API(install_jump_hook, "__gw_domain_jump_hook");
  API(install_context_hook, "__gw_domain_context_hook");
#endif
#undef API
  return 0;
}

static int quiesce_owner(void) {
  uint64_t all = UINT64_MAX, applied;
  if (syscall(SYS_rt_sigprocmask, SIG_SETMASK, &all, NULL, sizeof all) ||
      syscall(SYS_rt_sigprocmask, SIG_SETMASK, NULL, &applied,
              sizeof applied)) {
    return errno;
  }
  if (applied != GW_DOMAIN_STOP_MASK) {
    return EIO;
  }
  session->note_guest(0, 0, 0);
  session->owner_stopped_mask = applied;
  return 0;
}

static int stop_service(void) {
  full_guest_shutdown();
  int error = 0;
#ifdef GW_DOMAIN_NONLOCAL
  if (session->guest_notification) {
    error = gw_guest_workers_stop();
  }
#endif
  int stopped = quiesce_owner();
  return error ? error : stopped;
}

static _Noreturn void park_owner(void) {
  /* Do not resume full_guest_poll or invoke application code after STOP's
   * response. Host can stop its relay and restore its kernel handlers only
   * because this GNU thread can no longer receive catchable Host signals.
   * SIGKILL/SIGSTOP retain the kernel's process-wide semantics.
   * https://github.com/torvalds/linux/blob/v6.12/kernel/signal.c
   * See sigprocmask(), including sigdelsetmask(SIGKILL/SIGSTOP).
   *
   * The root TCB, loader and stack remain process-pinned. pthread_exit here
   * could treat the GNU root as the last pthread and exit the Host process.
   * No automatic process exit or speculative reclamation belongs here. */
  int parked = 0;
  for (;;) {
    syscall(SYS_futex, &parked, FUTEX_WAIT_PRIVATE, 0, NULL, NULL, 0);
  }
}

#ifdef GW_DOMAIN_NONLOCAL
static int setup_signals(void) {
  int helper = session->guest_notification;
  if (helper) {
    if (helper < SIGRTMIN || helper > SIGRTMAX) {
      return EINVAL;
    }
    int error = gw_guest_workers_start(helper, session->reserved_signals,
                                       session->quarantine_signals);
    if (error) {
      return error;
    }
    session->signal_cookie = gw_guest_signal_cookie();
    session->guest_virtual_signals = gw_guest_virtual_mask();
    if (session->domain) {
      gw_guest_signal_entries(&session->signal_entries);
      session->signal_invoke = gw_guest_signal_parked;
      session->signal_finish = gw_guest_signal_finish_parked;
      session->signal_resume = gw_domain_resume_signal;
    }
  } else {
    gw_guest_signals_init(NULL, 0);
  }

  if (install_sigaction_hook(gw_guest_sigaction) ||
      !session->bind_guest_kernel ||
      session->bind_guest_kernel(guest_kernel_entry)) {
    return EINVAL;
  }
  if (gw_domain_jump_init() || install_jump_hook(gw_domain_jump_event) ||
      install_context_hook(3, gw_domain_context_event)) {
    return EINVAL;
  }
  return 0;
}
#endif

int main(int argc, char **argv) {
  if (argc != 2) {
    _exit(124);
  }
  session = (void *)(uintptr_t)strtoull(argv[1], NULL, 16);
  session->note_guest(0, syscall(SYS_gettid),
                      (uintptr_t)__builtin_thread_pointer());
  struct jni_domain_handshake *hello = &session->handshake;
  uint64_t supported = GW_DOMAIN_CAP_ADOPTION | GW_DOMAIN_CAP_STOP_QUIESCENT;
#ifdef GW_DOMAIN_NONLOCAL
  supported |= GW_DOMAIN_CAP_NATIVE_UNWIND | GW_DOMAIN_CAP_WORKER_ASYNC |
               GW_DOMAIN_CAP_ADOPTED_ASYNC;
#endif
  if (hello->magic != GW_DOMAIN_TRANSPORT_MAGIC ||
      hello->version != GW_DOMAIN_TRANSPORT_VERSION ||
      hello->size != sizeof *session ||
      hello->context_size != sizeof(struct jni_domain_context) ||
      hello->exchange_size != sizeof(struct exchange) ||
      hello->siginfo_size != sizeof(siginfo_t) ||
      hello->ucontext_size != sizeof(ucontext_t) ||
      hello->sigmask_offset != offsetof(ucontext_t, uc_sigmask) ||
      hello->mcontext_offset != offsetof(ucontext_t, uc_mcontext) ||
      (hello->required & ~supported)) {
    session->error = EPROTO;
    goto reject;
  }
  session->error = resolve();
  if (session->error) {
    goto reject;
  }
  if (syscall(SYS_prctl, PR_SET_NAME, GW_DOMAIN_OWNER_NAME, 0, 0, 0)) {
    session->error = errno;
    goto reject;
  }
  hello->accepted = supported;
  if (install_thread_hook(session->guest_thread)) {
    session->error = EINVAL;
    goto reject;
  }
  if (reserve_signals(session->reserved_signals)) {
    session->error = EINVAL;
    goto reject;
  }
#ifdef GW_DOMAIN_NONLOCAL
  session->error = setup_signals();
  if (session->error) {
    goto reject;
  }
#endif
  full_guest_start(&session->shared);
#ifdef GW_DOMAIN_NONLOCAL
  if (session->guest_notification) {
    gw_guest_workers_unmask();
  }
#endif
  session->entry = adopted_entry;
  atomic_store_explicit(&session->shared.phase, 2, memory_order_release);
  atomic_store_explicit(&session->ready, 1, memory_order_release);
  wake_word(&session->ready);
  int sequence = 0;
  for (;;) {
    struct fb_event *event = &fb_context_at(G, 0)->guest_event;
    unsigned observed = fb_event_sample(event);
    int request = atomic_load_explicit(&session->request, memory_order_acquire);
    if (request != sequence) {
      struct jni_domain_context *c = session->context;
      int operation = session->operation;
      session->error = 0;
      if (operation == JNI_DOMAIN_ALLOC) {
        void *tp, *top;
        session->error = c->fail_allocate
                             ? ENOMEM
                             : allocate_domain((void *)c->exchange.stack_low,
                                               c->exchange.stack_high -
                                                   c->exchange.stack_low,
                                               &c->token, &tp, &top);
        if (!session->error) {
          c->exchange.guest.tp = (uintptr_t)tp;
          c->exchange.guest.sp = (uintptr_t)top & ~(uintptr_t)15;
        }
      } else if (operation == JNI_DOMAIN_FREE) {
        session->error = free_domain(c->token);
      } else if (operation == JNI_DOMAIN_STOP) {
        session->error = stop_service();
      } else {
        session->error = EINVAL;
      }
      sequence = request;
      atomic_store_explicit(&session->response, sequence, memory_order_release);
      wake_word(&session->response);
      if (operation == JNI_DOMAIN_STOP) {
        park_owner();
      }
    }
    full_guest_poll();
    fb_event_wait(event, observed);
  }
reject:
  atomic_store_explicit(&session->ready, 1, memory_order_release);
  wake_word(&session->ready);
  /* Host owns the session mapping, including after a failed handshake. */
  syscall(SYS_exit, 123);
  __builtin_unreachable();
}
