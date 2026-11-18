#define _GNU_SOURCE
#include "signals.h"
#include "signal-stack.h"
#include "signal-relay.h"
#include "raw_syscall.h"
#include "nonlocal-mask.h"
#include "nonlocal.h"
#include "signal-execution.h"
#include "signal-targets.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <string.h>
#include <sys/signalfd.h>
#if defined(__aarch64__)
#include <asm/sigcontext.h>
#endif

/* Snapshot only state not restored by the owning libc's native longjmp. The
 * original kernel frame is authoritative; a last cooperative swap can predate
 * a module's fenv changes. Host signal execution must not overwrite it.
 * https://github.com/torvalds/linux/blob/v6.12/arch/arm64/include/uapi/asm/sigcontext.h
 * https://github.com/torvalds/linux/blob/v6.12/arch/x86/include/uapi/asm/sigcontext.h
 */
static void read_control(ucontext_t *context, uint64_t control[3]) {
#if defined(__x86_64__)
  if (!context->uc_mcontext.fpregs) {
    _exit(96);
  }
  control[0] = context->uc_mcontext.fpregs->mxcsr;
  control[1] = context->uc_mcontext.fpregs->cwd;
#else
  control[2] = context->uc_mcontext.regs[18];
  unsigned char *records = context->uc_mcontext.__reserved;
  size_t offset = 0, size = sizeof context->uc_mcontext.__reserved;
  while (size - offset >= sizeof(struct _aarch64_ctx)) {
    struct _aarch64_ctx *head = (void *)(records + offset);
    if (head->size < sizeof *head || head->size > size - offset) {
      break;
    }
    if (head->magic == FPSIMD_MAGIC &&
        head->size >= sizeof(struct fpsimd_context)) {
      struct fpsimd_context *fp = (void *)head;
      control[0] = fp->fpcr;
      control[1] = fp->fpsr;
      return;
    }
    if (head->size % 16) {
      break;
    }
    offset += head->size;
  }
  _exit(96);
#endif
}

static void save_guest_control(struct exchange *exchange, ucontext_t *context) {
  _Static_assert(sizeof context->uc_sigmask >= sizeof(uint64_t),
                 "kernel signal mask representation");
  memcpy(&exchange->interrupted_guest_mask, &context->uc_sigmask,
         sizeof exchange->interrupted_guest_mask);
  gw_signal_stack_get(&exchange->interrupted_guest_altstack);
  read_control(context, exchange->interrupted_control);
}

struct signal_slot signal_slots[DOMAIN_FIXTURE_CARRIERS];
/* Only access after Host TP restoration. Assembly still uses the stable
 * TLS-free registry, while Host mask/wait APIs avoid scanning that registry. */
static _Thread_local struct signal_slot *host_recipient;

struct routed_guest;

struct host_signal_anchor {
  struct host_signal_anchor *previous;
  struct host_signal_anchor *volatile live_next;
  struct signal_slot *slot;
  ucontext_t *frame;
  uint64_t mask, control[3];
  const struct domain *suspended;
  void *guest_identity;
  struct gw_nonlocal_scope *scope;
  struct routed_guest *guest_route;
  void *volatile wait;
};
/* Accessed after assembly restores Host TLS. The entry owns each anchor until
 * returning to its kernel frame or retiring it through native recovery. */
static _Thread_local struct host_signal_anchor *host_anchor;
static _Atomic unsigned deferred_recipients;
extern int gw_signal_selection_interrupt(void *, void *, uint64_t *)
    __attribute__((weak));
extern int gw_signal_selection_hidden(void *, uint64_t *) __attribute__((weak));

#ifdef GW_DOMAIN_CHAIN
static _Atomic unsigned independent_entries;

struct routed_guest {
  struct routed_guest *previous;
  struct signal_slot *slot;
  struct gw_signal_suspension suspended;
  struct gw_nonlocal_scope unwind;
  uint64_t control[3];
  void *identity;
  int (*volatile resume)(int, void *, void *);
};

static _Thread_local struct routed_guest *routed_guest;
static _Thread_local struct host_signal_anchor *live_anchors;
extern void *gw_signal_wait_scope(void) __attribute__((weak));
extern void gw_signal_wait_select(void *) __attribute__((weak));

/* Called from the active GNU entry via the TLS-switch assembly. The two
 * views identify live frames, including any intervening Host callback.
 * Selecting a view does not run a landing or release its resources. */
int signal_guest_select_host(int unused, void *from, void *to) {
  (void)unused;
  struct gw_signal_execution *source = from, *target = to;
  struct host_signal_anchor *old = source->host_view;
  struct host_signal_anchor *next = target->host_view;
  if (!old || old != host_anchor || !next || old->slot != next->slot ||
      gw_nonlocal_select(old->scope, next->scope)) {
    return EBUSY;
  }

  if (gw_signal_wait_scope && gw_signal_wait_select) {
    old->wait = gw_signal_wait_scope();
    gw_signal_wait_select(next->wait);
  }
  host_anchor = next;
  routed_guest = next->guest_route;
  return 0;
}

int signal_guest_prepare_retire(int unused, void *argument, void *ignored) {
  (void)unused;
  (void)ignored;
  struct gw_signal_retirement *plan = argument;
  if (plan->source == plan->leaf || plan->leaf->retirement ||
      !plan->parent->host_view) {
    return EINVAL;
  }

  gw_nonlocal_suspend_transfer(&plan->saved_transfer);
  int error = signal_guest_select_host(0, plan->source, plan->leaf);
  if (error) {
    gw_nonlocal_restore_transfer(&plan->saved_transfer);
    return error;
  }
  plan->leaf->retirement = plan;
  return 0;
}

static void resume_after_retirement(void *argument) {
  struct gw_signal_retirement *plan = argument;
  if (plan->parent != plan->source &&
      signal_guest_select_host(0, plan->parent, plan->source)) {
    _exit(125);
  }
  gw_nonlocal_restore_transfer(&plan->saved_transfer);

  /* Preserve the surviving Host frame's original continuation. The Guest
   * will return to it after the leaf and its intervening callbacks are gone. */
  struct domain discarded;
  plan->source->swap(&discarded, &plan->source->guest);
  _exit(125);
}

static void retire_host_anchor(struct host_signal_anchor *anchor) {
  struct host_signal_anchor *volatile *link = &live_anchors;
  while (*link != anchor) {
    if (!*link || (*link)->previous == anchor) {
      _exit(125);
    }
    link = &(*link)->live_next;
  }
  *link = anchor->live_next;
}

int signal_guest_suspended(struct signal_slot *slot,
                           struct gw_signal_suspension *suspended) {
  if (!routed_guest || routed_guest->slot != slot ||
      !routed_guest->suspended.stack_floor) {
    return 0;
  }
  *suspended = routed_guest->suspended;
  return 1;
}

_Noreturn void signal_guest_resume(void *identity,
                                   int (*restore)(int, void *, void *)) {
  struct routed_guest *entry = routed_guest;
  while (entry && entry->identity != identity) {
    entry = entry->previous;
  }
  if (!identity || !entry || !restore) {
    _exit(125);
  }
  entry->resume = restore;
  gw_nonlocal_route(&entry->unwind);
}

static void restore_execution_stack(const stack_t *desired) {
  stack_t actual;
  gw_signal_stack_get(&actual);
  if (actual.ss_sp != desired->ss_sp || actual.ss_size != desired->ss_size ||
      actual.ss_flags != desired->ss_flags) {
    gw_signal_stack_set(desired);
  }
}

int signal_guest_execute(struct gw_signal_execution *execution,
                         void *host_frame) {
  struct signal_slot *slot = host_recipient;
  if (!execution || execution->version != GW_SIGNAL_EXECUTION_VERSION ||
      !slot || execution->guest.tp != slot->exchange->guest.tp ||
      execution->abort.tp != execution->guest.tp ||
      execution->phase != GW_SIGNAL_EXECUTION_PREPARED || !execution->swap) {
    return EINVAL;
  }
  if (!gw_nonlocal_ready()) {
    return ENOTSUP;
  }

  uint64_t incoming_mask = gw_nonlocal_mask_block();
  struct host_signal_anchor anchor = {.previous = host_anchor,
                                      .slot = slot,
                                      .frame = host_frame,
                                      .suspended = &execution->host,
                                      .guest_identity = execution->argument};
  if (host_frame) {
    memcpy(&anchor.mask, &anchor.frame->uc_sigmask, sizeof anchor.mask);
    if (gw_signal_selection_hidden) {
      gw_signal_selection_hidden(host_frame, &anchor.mask);
    }
    anchor.mask = signal_host_logical_mask(anchor.mask);
    read_control(anchor.frame, anchor.control);
    if (execution->carrier_mask) {
      *execution->carrier_mask = signal_carrier_mask(slot, anchor.mask, 1);
    }
  }
  stack_t original_stack;
  gw_signal_stack_get(&original_stack);
  struct gw_nonlocal_scope scope;
  gw_nonlocal_enter(&scope);
  atomic_fetch_add_explicit(&independent_entries, 1, memory_order_acquire);
  if (host_frame) {
    anchor.scope = &scope;
    anchor.guest_route = routed_guest;
    anchor.wait = gw_signal_wait_scope ? gw_signal_wait_scope() : NULL;
    anchor.live_next = live_anchors;
    live_anchors = &anchor;
    execution->host_view = &anchor;
    host_anchor = &anchor;
  }

  if (!sigsetjmp(scope.landing, 0)) {
    execution->swap(&execution->host, &execution->guest);
    restore_execution_stack(&original_stack);
    if (execution->retirement) {
      struct gw_signal_retirement *plan = execution->retirement;
      struct host_signal_anchor *parent = plan->parent->host_view;
      gw_nonlocal_retire_to(parent->scope, resume_after_retirement, plan);
    }
  } else {
    /* A nested Host handler may have changed its own alternate stack before
     * the jump. Keep that actual Host state across Guest-only retirement. */
    stack_t retained_stack;
    gw_signal_stack_get(&retained_stack);
    execution->swap(&execution->host, &execution->abort);
    restore_execution_stack(&retained_stack);
  }
  if (host_frame) {
    retire_host_anchor(&anchor);
    execution->host_view = NULL;
    host_anchor = anchor.previous;
  }
  int result = execution->phase == GW_SIGNAL_EXECUTION_DONE ? execution->result
                                                            : ECANCELED;
  if (execution->release_host) {
    execution->release_host(execution->argument);
  }
  atomic_fetch_sub_explicit(&independent_entries, 1, memory_order_release);
  gw_nonlocal_leave(&scope);
  gw_nonlocal_mask_restore(incoming_mask);

  return result;
}

static uint64_t suspended_host_mask(struct signal_slot *slot) {
  return host_anchor && host_anchor->slot == slot
             ? host_anchor->mask
             : slot->exchange->host_execution_mask;
}
#endif

static void restore_anchored_control(struct signal_slot *slot) {
  if (!host_anchor || host_anchor->slot != slot) {
    return;
  }
  const uint64_t *control = host_anchor->control;
#if defined(__x86_64__)
  __asm__ volatile("ldmxcsr %0" : : "m"(control[0]));
  __asm__ volatile("fldcw %0" : : "m"(control[1]));
#else
  __asm__ volatile("msr fpcr, %0" : : "r"(control[0]));
  __asm__ volatile("msr fpsr, %0" : : "r"(control[1]));
  __asm__ volatile("mov x18, %0" : : "r"(control[2]));
#endif
}

unsigned signal_deferred_recipients(void) {
  return atomic_load_explicit(&deferred_recipients, memory_order_acquire);
}

static int signal_try_register(struct signal_slot *, struct exchange *);

static void publish_recipient(struct signal_slot *slot) {
#ifdef GW_DOMAIN_CHAIN
  uint64_t mask = gw_nonlocal_mask_block();
  host_recipient = slot;
  /* Registration's mask wrapper ran before TLS named this recipient and kept
   * private lanes blocked. Publish first, then apply this recipient's logical
   * mask; otherwise its first notification can remain blocked indefinitely. */
  mask = signal_host_physical_mask(signal_host_logical_mask(mask));
  gw_nonlocal_mask_restore(mask);
#else
  host_recipient = slot;
#endif
}

#include "signal-registry.inc"
extern void domain_signal_entry(int, siginfo_t *, void *);
#ifdef GW_DOMAIN_CHAIN
#include "signal-router.h"
#include "nonlocal-mask.h"
_Atomic int domain_forwarded_faults;
_Atomic int domain_forwarded_async;
_Atomic int domain_guest_host_signals;
_Atomic int domain_expected_host_signals;
_Atomic int domain_sr_deliveries;
_Atomic int domain_sr_guest_deliveries;
static _Atomic long guest_tids[DOMAIN_FIXTURE_CARRIERS + 1];
static _Atomic uintptr_t guest_tps[DOMAIN_FIXTURE_CARRIERS + 1];
static _Atomic(uintptr_t) guest_kernel_entry;
static _Atomic int guest_kernel_notification;

void signal_guest_notification_discard(void *context) {
  if (gw_signal_selection_hidden) {
    uint64_t unused;
    gw_signal_selection_hidden(context, &unused);
  }
}

static void ignore_guest_notification(int number, siginfo_t *info,
                                      void *context) {
  (void)number;
  (void)info;
  /* Ordinary Host recipients cannot consume a native GNU queue. */
  signal_guest_notification_discard(context);
}

int signal_guest_kernel_notify_enable(int number,
                                      void (*handler)(int, siginfo_t *,
                                                      void *)) {
  if (atomic_load_explicit(&guest_kernel_notification, memory_order_acquire)) {
    return EALREADY;
  }
  int error = signal_router_internal_action(
      number, handler ? handler : ignore_guest_notification);
  if (!error) {
    atomic_store_explicit(&guest_kernel_notification, number,
                          memory_order_release);
  }
  return error;
}

static uint64_t guest_notification_bit(void) {
  int number =
      atomic_load_explicit(&guest_kernel_notification, memory_order_acquire);
  return number ? UINT64_C(1) << (number - 1) : 0;
}

int signal_guest_carrier_attach(struct signal_slot *slot) {
  if (!slot || slot != host_recipient || !guest_notification_bit() ||
      !slot->deferred_active) {
    return EINVAL;
  }
  if (slot->guest_async) {
    return EALREADY;
  }
  pthread_attr_t attributes;
  int error = pthread_getattr_np(pthread_self(), &attributes);
  if (error) {
    return error;
  }
  void *stack;
  size_t size;
  error = pthread_attr_getstack(&attributes, &stack, &size);
  pthread_attr_destroy(&attributes);
  if (error) {
    return error;
  }
  slot->host_stack_low = (uintptr_t)stack;
  slot->host_stack_high = slot->host_stack_low + size;
  slot->guest_async = 1;
  return 0;
}

int signal_guest_carrier_detach(struct signal_slot *slot) {
  if (!slot || slot != host_recipient || !slot->guest_async) {
    return EINVAL;
  }
  uint64_t wanted = guest_notification_bit();
  struct timespec timeout = {0};
  int saved = errno;
  while (syscall(SYS_rt_sigtimedwait, &wanted, NULL, &timeout, sizeof wanted) >
         0) {
  }
  int error = errno == EAGAIN ? 0 : errno;
  errno = saved;
  if (!error) {
    slot->guest_async = 0;
  }
  return error;
}

static _Atomic(struct gw_signal_target *(*)(void)) guest_signal_target;

static __attribute__((no_stack_protector)) void note_host_interruption(
    struct gw_signal_target *target, int selected, int restart) {
  if (target) {
    atomic_fetch_add_explicit(&target->host_wakes, 1, memory_order_relaxed);
    if (selected) {
      atomic_fetch_add_explicit(&target->host_deliveries, 1,
                                memory_order_relaxed);
      if (!restart) {
        atomic_fetch_add_explicit(&target->host_interruptions, 1,
                                  memory_order_relaxed);
      }
    }
  }
}

/* The pinned GNU service installs this before admitting module constructors
 * or pthreads. Called with Guest TP; no Host libc, TLS or errno access. */
__attribute__((no_stack_protector)) int signal_guest_kernel_bind(
    void (*entry)(int, void *, void *),
    struct gw_signal_target *(*target)(void)) {
  uintptr_t expected = 0;
  if (!entry || !target ||
      !atomic_compare_exchange_strong_explicit(
          &guest_kernel_entry, &expected, (uintptr_t)entry,
          memory_order_release, memory_order_relaxed)) {
    return EINVAL;
  }
  atomic_store_explicit(&guest_signal_target, target, memory_order_release);
  return 0;
}

/* TLS/libc-free selection before the assembly tail transfer. A GNU unwind
 * cannot depend on FDEs owned only by Bionic's separate loader namespace.
 * The owning Guest handler returns to the original kernel restorer directly.
 * https://github.com/gcc-mirror/gcc/blob/releases/gcc-14/libgcc/config/aarch64/linux-unwind.h
 * https://github.com/gcc-mirror/gcc/blob/releases/gcc-14/libgcc/config/i386/linux-unwind.h
 */
__attribute__((no_stack_protector)) uintptr_t
domain_guest_thread_select(int number, const siginfo_t *information) {
  if (number == atomic_load_explicit(&guest_kernel_notification,
                                     memory_order_acquire) ||
      (information->si_code > 0 &&
       (number == SIGSEGV || number == SIGBUS || number == SIGILL ||
        number == SIGFPE || number == SIGTRAP || number == SIGSYS))) {
    return atomic_load_explicit(&guest_kernel_entry, memory_order_acquire);
  }
  return 0;
}

/* Called with adopted Guest TLS before the assembly can switch to Host.
 * Synchronous faults still use their existing Host recovery coordinator. */
__attribute__((no_stack_protector, visibility("hidden"))) uintptr_t
domain_guest_notify_select(int number) {
  return number == atomic_load_explicit(&guest_kernel_notification,
                                        memory_order_acquire)
             ? atomic_load_explicit(&guest_kernel_entry, memory_order_acquire)
             : 0;
}

/* Genuine GNU pthreads keep their real TLS. Hardware faults stay in Guest;
 * asynchronous Host handlers explicitly run on the real Host dispatcher. */
__attribute__((no_stack_protector)) void domain_guest_thread_dispatch(
    int number, siginfo_t *info, void *context, void *token, uint64_t mask) {
  if (number ==
      atomic_load_explicit(&guest_kernel_notification, memory_order_acquire)) {
    return; /* Native entry is not yet bound. No Guest sender is admitted. */
  }
  if (info->si_code > 0 &&
      (number == SIGSEGV || number == SIGBUS || number == SIGILL ||
       number == SIGFPE || number == SIGTRAP || number == SIGSYS)) {
    /* No matching Guest entry was configured. A Host VM handler must never
     * interpret this foreign GNU execution frame as its own fault. */
    gw_raw3(SYS_exit_group, 96, 0, 0);
    __builtin_unreachable();
  }
  const ucontext_t *frame = context;
  uint64_t interrupted;
  __builtin_memcpy(&interrupted, &frame->uc_sigmask, sizeof interrupted);
  mask = signal_router_entry_mask(token, mask, interrupted);
  int selected;
  if (!signal_host_relay(number, info, context, token, mask, &selected)) {
    struct gw_signal_target *(*target)(void) =
        atomic_load_explicit(&guest_signal_target, memory_order_acquire);
    if (target) {
      note_host_interruption(target(), selected,
                             signal_router_token_restart(token));
    }
    return;
  }

  gw_raw3(SYS_exit_group, 96, 0, 0);
  __builtin_unreachable();
}

/* Callable with Guest TLS: no libc, allocation, locks or stack protector. */
__attribute__((no_stack_protector)) void signal_note_guest_thread(
    int index, long tid, uintptr_t tp) {
  atomic_store_explicit(&guest_tps[index], tp, memory_order_relaxed);
  atomic_store_explicit(&guest_tids[index], tid, memory_order_release);
}

static struct gw_host_stack_floors host_stack_floors(
    const struct host_signal_anchor *anchor) {
  const stack_t *alternate = &anchor->frame->uc_stack;
  uintptr_t original = anchor->suspended->sp;
  struct gw_host_stack_floors floors = {0};
  if (gw_stack_contains(alternate, original)) {
    floors.alternate = original;
  } else {
    floors.ordinary = original;
  }
  if (!anchor->slot->guest_async) {
    return floors;
  }

  uintptr_t interrupted_sp = gw_signal_interrupted_sp(anchor->frame);
  uintptr_t low = anchor->slot->host_stack_low;
  uintptr_t high = anchor->slot->host_stack_high;
  for (const struct host_signal_anchor *entry = live_anchors; entry;
       entry = entry->live_next) {
    uintptr_t sp = entry->suspended->sp;
    if (entry->slot != anchor->slot || !sp) {
      continue;
    }
    if (gw_stack_contains(alternate, sp)) {
      if (!floors.alternate || sp < floors.alternate) {
        floors.alternate = sp;
      }
    } else if (interrupted_sp >= low && interrupted_sp < high && sp >= low &&
               sp < high && (!floors.ordinary || sp < floors.ordinary)) {
      floors.ordinary = sp;
    }
  }
  return floors;
}

static void forward_anchored_host(int number, siginfo_t *info, void *token,
                                  int selected, uintptr_t *guest_floor) {
  /* A VM handler must see the real suspended Host PC/registers, never the
   * Guest handler's kernel context or the last cooperative call's SP. */
  ucontext_t *frame = host_anchor->frame;
  uint64_t mask;
  memcpy(&mask, &frame->uc_sigmask, sizeof mask);
  mask = signal_host_logical_mask(mask);
  memcpy(&frame->uc_sigmask, &mask, sizeof mask);
  struct gw_host_stack_floors floors = host_stack_floors(host_anchor);
  signal_router_forward_selected_suspended(
      number, info, frame, token, host_anchor->mask, &frame->uc_stack,
      gw_signal_interrupted_sp(frame), &floors, selected, guest_floor);
  memcpy(&mask, &frame->uc_sigmask, sizeof mask);
  mask = signal_host_physical_mask(mask);
  memcpy(&frame->uc_sigmask, &mask, sizeof mask);
}

static void deliver_foreign_host(int number, siginfo_t *info, void *context,
                                 void *token, struct signal_slot *slot,
                                 int selected, uintptr_t *guest_floor) {
  if (host_anchor && host_anchor->slot == slot) {
    forward_anchored_host(number, info, token, selected, guest_floor);
  } else {
    struct exchange *exchange = slot->exchange;
    signal_router_forward_selected_suspended(
        number, info, context, token, exchange->host_execution_mask,
        &exchange->host_altstack, exchange->host.sp, NULL, selected,
        guest_floor);
  }
}

static _Noreturn void resume_interrupted_guest(
    const struct routed_guest *entry) {
  restore_execution_stack(&entry->suspended.alternate);
  struct domain_signal_call call = {
      .entry = entry->resume,
      .tp = entry->suspended.state.tp,
      .x18 = entry->control[2],
      .control = {entry->control[0], entry->control[1]},
      .context = entry->suspended.frame};
  domain_guest_signal_call(&call);
  _exit(125);
}

static void forward_foreign_host(int number, siginfo_t *info, void *context,
                                 void *token, struct signal_slot *slot,
                                 int selected) {
  if (!slot->guest_async || selected != 1) {
    deliver_foreign_host(number, info, context, token, slot, selected, NULL);
    return;
  }

  /* A directed Guest notification can land during the Host handler. Publish
   * the actual interrupted Guest frame, even when it is younger than the
   * cooperative RPC frame. Retire this publication on Host-native escapes. */
  struct routed_guest entry = {.previous = routed_guest, .slot = slot};
  if (host_anchor && host_anchor->slot == slot) {
    entry.identity = host_anchor->guest_identity;
  }
  entry.suspended.frame = context;
  entry.suspended.state = slot->exchange->guest;
  gw_signal_stack_get(&entry.suspended.alternate);
  read_control(context, entry.control);
#if defined(__aarch64__)
  entry.suspended.state.saved[12] = entry.control[2];
  entry.suspended.state.saved[21] = entry.control[0];
  entry.suspended.state.saved[22] = entry.control[1];
#else
  entry.suspended.state.saved[6] = entry.control[0];
  entry.suspended.state.saved[7] = entry.control[1];
#endif

  gw_nonlocal_enter(&entry.unwind);
  routed_guest = &entry;
  if (!sigsetjmp(entry.unwind.landing, 0)) {
    deliver_foreign_host(number, info, context, token, slot, selected,
                         &entry.suspended.stack_floor);
  }
  routed_guest = entry.previous;
  gw_nonlocal_leave(&entry.unwind);
  if (entry.resume) {
    resume_interrupted_guest(&entry);
  }
}

static void forward_host(int number, siginfo_t *info, void *context,
                         void *token, struct signal_slot *slot,
                         uintptr_t interrupted_tp, uint64_t entry_mask,
                         int selected) {
  uint64_t execution = entry_mask;
  if (selected < 0) {
    selected = signal_router_select(number, info, token);
  }
  if (slot && interrupted_tp != slot->host_tp) {
    note_host_interruption(slot->exchange->guest_signal_target, selected,
                           signal_router_token_restart(token));
  }
  void *selection_frame = slot && interrupted_tp != slot->host_tp &&
                                  host_anchor && host_anchor->slot == slot
                              ? host_anchor->frame
                              : context;
  int mask_wait =
      !selected && gw_signal_selection_hidden
          ? gw_signal_selection_hidden(selection_frame, &execution)
      : gw_signal_selection_interrupt
          ? gw_signal_selection_interrupt(token, selection_frame, &execution)
          : 0;

  if (number == SIGUSR2) {
    atomic_fetch_add_explicit(&domain_sr_deliveries, 1, memory_order_relaxed);
  }
  if (number == SIGSEGV) {
    atomic_fetch_add_explicit(&domain_forwarded_faults, 1,
                              memory_order_relaxed);
  } else {
    atomic_fetch_add_explicit(&domain_forwarded_async, 1, memory_order_relaxed);
  }
  if (slot && interrupted_tp != slot->host_tp &&
      ((host_anchor && host_anchor->slot == slot) ||
       slot->exchange->host_mask_valid)) {
    forward_foreign_host(number, info, context, token, slot, selected);
  } else {
    ucontext_t *frame = context;
    uint64_t mask;
    memcpy(&mask, &frame->uc_sigmask, sizeof mask);
    if (signal_router_fault_notifications()) {
      /* A native Host handler may inspect or edit this saved mask. Private
       * notification bits are an implementation detail even for unrelated
       * Host signals; expose the logical mask and translate edits on return.
       * Nonlocal exits consume the logical snapshot in their owning libc. */
      mask = signal_host_logical_mask(mask);
      memcpy(&frame->uc_sigmask, &mask, sizeof mask);
    }
    /* A mask-wait's ucontext saves the mask to restore after the wait, not
     * necessarily the temporary mask under which this handler must execute.
     * Assembly captured the actual kernel handler-entry mask before blocking.
     * https://github.com/torvalds/linux/blob/v6.12/include/linux/sched/signal.h
     * See sigmask_to_save(), including current->saved_sigmask. */
    int wide = signal_router_fault_notifications() &&
               (gw_signal_fault_bits() & (UINT64_C(1) << (number - 1)));
    uint64_t interrupted = mask_wait ? execution : mask;
    uint64_t effective = signal_host_logical_mask(
        wide ? interrupted
             : signal_router_entry_mask(token, entry_mask, interrupted));
    signal_router_forward_selected_domain(
        number, info, context, token, effective, &frame->uc_stack,
        gw_signal_interrupted_sp(frame), 0, selected);
    if (signal_router_fault_notifications()) {
      memcpy(&mask, &frame->uc_sigmask, sizeof mask);
      mask = signal_host_physical_mask(mask);
      memcpy(&frame->uc_sigmask, &mask, sizeof mask);
    }
  }
}

static uint64_t synchronize_deferred(struct signal_slot *);

static struct signal_slot *host_slot(void) {
  return host_recipient;
}

int signal_host_stack_bounds(uintptr_t *low, size_t *size) {
  struct signal_slot *slot = host_slot();
  if (!slot || !slot->host_stack_low) {
    return 0;
  }
  *low = slot->host_stack_low;
  *size = slot->host_stack_high - slot->host_stack_low;
  return 1;
}

struct gw_signal_deferred *signal_host_consumer(void) {
  struct signal_slot *slot = host_slot();
  if (!slot || !slot->deferred_active) {
    return NULL;
  }

  synchronize_deferred(slot);
  gw_signal_deferred_prune(slot->deferred);
  return slot->deferred;
}

int signal_host_consumer_fd(void) {
  struct signal_slot *slot = host_slot();
  return slot && slot->deferred_active ? slot->consume_fd : -1;
}

/* Recipient-local maintenance, with catchable signals blocked. Registration
 * never follows foreign TLS or races with unmapping another recipient's queue.
 * An ignore generation survives a later handler registration, so old pending
 * work cannot become visible again merely because this thread was suspended.
 * Linux discards pending instances when installing SIG_IGN:
 * https://github.com/torvalds/linux/blob/v6.12/kernel/signal.c
 * See do_sigaction(), including flush_sigqueue_mask().
 */
static uint64_t synchronize_deferred_at(struct signal_slot *slot,
                                        int delivering, void *token) {
  uint64_t ignored = 0;
  if (!slot || !slot->deferred_active) {
    return 0;
  }
  uint64_t pending = 0;
  int inspected = 0;
  const int faults[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGSYS};
  for (int i = 0; i < GW_SIGNAL_FAULT_LANES; ++i) {
    int discard;
    uint64_t generation = signal_router_ignore_generation(faults[i], &discard);
    if (discard || generation != slot->deferred_ignore_generation[i]) {
      uint64_t original = UINT64_C(1) << (faults[i] - 1);
      int consumed = 0;
      if ((slot->deferred->armed & original) &&
          (gw_signal_pending_bits(&slot->deferred->pending) & original)) {
        if (!inspected &&
            syscall(SYS_rt_sigpending, &pending, sizeof pending)) {
          _exit(125);
        }
        inspected = 1;
        int level = gw_signal_deferred_level(slot->deferred, i);
        /* SIG_IGN flushes promoted original numbers in the kernel. Their
         * absence alone does not prove selection; only the matching immutable
         * delivery token can retain an already selected original packet. */
        consumed = !(slot->deferred->promoted & original) &&
                   !(pending & (UINT64_C(1) << (level - 1)));
        if (delivering == level) {
          /* A matching immutable token owns the old selected packet, even if
           * a fresh original arrived after its kernel dequeue. */
          consumed = signal_router_token_ignore_generation(token) ==
                     slot->deferred_ignore_generation[i];
        }
      }
      /* SIG_IGN discards pending signals, not data already dequeued by a
       * native reader. Its result can still be in the consumption window. */
      if (!consumed) {
        /* Installing SIG_IGN already flushed the old native level. A fresh
         * native packet after restoration must not be drained as its wake. */
        if (slot->deferred->promoted & original) {
          slot->deferred->promoted &= ~original;
          slot->deferred->armed &= ~original;
        }
        gw_signal_deferred_discard(slot->deferred, faults[i]);
      }
      slot->deferred_ignore_generation[i] = generation;
    }
    if (discard) {
      ignored |= UINT64_C(1) << (faults[i] - 1);
    }
  }
  return ignored;
}

static uint64_t synchronize_deferred(struct signal_slot *slot) {
  return synchronize_deferred_at(slot, 0, NULL);
}

int signal_host_defer(struct signal_slot *slot, const siginfo_t *information) {
  if (!slot || slot != host_slot() || !slot->deferred_active || !information ||
      information->si_signo < 1 || information->si_signo > 64) {
    return EINVAL;
  }
  uint64_t previous = gw_nonlocal_mask_block();
  uint64_t ignored = synchronize_deferred(slot);
  int result = (ignored & (UINT64_C(1) << (information->si_signo - 1)))
                   ? 0
                   : gw_signal_deferred_put(slot->deferred, information);
  synchronize_deferred(slot);
  gw_nonlocal_mask_restore(previous);
  return result;
}

uint64_t signal_carrier_mask(struct signal_slot *slot, uint64_t logical,
                             int guest) {
#ifdef __GLIBC__
  /* The Host cancellation handler can force-unwind a native Host stack.
   * Deliver it only after the carrier has restored its Host thread pointer.
   * https://github.com/bminor/glibc/blob/glibc-2.42/nptl/pthread_cancel.c */
  if (guest) {
    logical |= UINT64_C(1) << (GW_GNU_HOST_CANCEL - 1);
  }
#endif
  if (slot && slot->guest_async) {
    logical &= ~guest_notification_bit();
  }
  if (!slot || !slot->deferred_active) {
    return logical;
  }
  if (guest) {
    logical &= ~gw_signal_fault_bits();
  } else if (gw_signal_pending_bits(&slot->deferred->pending) & ~logical &
             ~slot->deferred->promoted) {
    uint64_t mask = signal_router_selection_begin();
    synchronize_deferred(slot);
    gw_signal_deferred_prune(slot->deferred);
    int error = gw_signal_deferred_promote(slot->deferred, ~logical);
    if (error) {
      signal_router_quarantine();
    }
    signal_router_selection_end(mask);
  }
  return gw_signal_deferred_mask(slot->deferred, logical, guest);
}

uint64_t signal_host_logical_mask(uint64_t physical) {
  physical &= ~guest_notification_bit();
  const int *notifications = signal_router_fault_notifications();
  if (notifications) {
    for (int i = 0; i < GW_SIGNAL_FAULT_LANES; ++i) {
      physical &= ~(UINT64_C(1) << (notifications[i] - 1));
    }
  }
  return physical;
}

uint64_t signal_host_physical_mask(uint64_t logical) {
  if (!signal_router_fault_notifications()) {
    return logical;
  }
  struct signal_slot *slot = host_slot();
  if (slot && slot->deferred_active) {
    signal_host_prune_notifications();
    return signal_carrier_mask(slot, logical, 0);
  }
  logical |= guest_notification_bit();
  /* Before carrier registration there can be no notification for this thread.
   * Keep the private signals blocked until its queue and lookup are published.
   */
  const int *notifications = signal_router_fault_notifications();
  for (int i = 0; i < GW_SIGNAL_FAULT_LANES; ++i) {
    logical |= UINT64_C(1) << (notifications[i] - 1);
  }
  return logical;
}

uint64_t signal_host_pending(uint64_t physical) {
  struct signal_slot *slot = host_slot();
  uint64_t previous = gw_nonlocal_mask_block();
  synchronize_deferred(slot);
  uint64_t result = slot && slot->deferred_active
                        ? gw_signal_deferred_visible(slot->deferred, physical)
                        : signal_host_logical_mask(physical);
  gw_nonlocal_mask_restore(previous);
  return result & ~guest_notification_bit();
}

uint64_t signal_host_wait_mask(uint64_t wanted) {
  const int *notifications = signal_router_fault_notifications();
  wanted = signal_host_logical_mask(wanted);
  /* A signalfd belongs to an open file description, not its creating thread.
   * Include lanes even before registration: another recipient can read it. */
  return notifications ? gw_signal_notification_mask(notifications, wanted)
                       : wanted;
}

void signal_host_prune_notifications(void) {
  struct signal_slot *slot = host_slot();
  if (slot && slot->deferred_active) {
    uint64_t previous = gw_nonlocal_mask_block();
    synchronize_deferred(slot);
    gw_signal_deferred_prune(slot->deferred);
    gw_nonlocal_mask_restore(previous);
  }
}

int signal_host_take_notification(int number, siginfo_t *information) {
  const int *notifications = signal_router_fault_notifications();
  if (!notifications) {
    return -1;
  }
  for (int i = 0; i < GW_SIGNAL_FAULT_LANES; ++i) {
    if (notifications[i] == number) {
      struct signal_slot *slot = host_slot();
      synchronize_deferred(slot);
      return slot && slot->deferred_active
                 ? gw_signal_deferred_take(slot->deferred, number, information)
                 : 0;
    }
  }
  return -1;
}

int signal_host_collect_notification(int number, siginfo_t *information) {
  struct signal_slot *slot = host_slot();
  if (!slot || !slot->deferred_active) {
    return -1;
  }
  synchronize_deferred(slot);
  return gw_signal_deferred_collect(slot->deferred, number, information);
}

int signal_host_retirement_busy(void) {
  /* This is a busy test, never proof of recipient/continuation quiescence.
   * Entry admission must be frozen separately before retiring the runtime. */
  if (atomic_load_explicit(&independent_entries, memory_order_acquire)) {
    return 1;
  }
  struct signal_slot *slot = host_slot();
  if (!slot || !slot->deferred_active) {
    return 0;
  }

  uint64_t mask = signal_router_selection_begin();
  synchronize_deferred(slot);
  gw_signal_deferred_prune(slot->deferred);
  int busy =
      slot->deferred->armed || gw_signal_pending_bits(&slot->deferred->pending);
  signal_router_selection_end(mask);
  return busy;
}

/* Original-number entries have a full outer mask. Bind their logical packet
 * before exposing any user mask; a post-dequeue native send remains a separate
 * kernel pending instance. The kernel-selected action token is immutable. */
static int dispatch_promoted(int number, siginfo_t *info, void *context,
                             struct signal_slot *slot, uintptr_t interrupted_tp,
                             uint64_t entry_mask, void *kernel_token) {
  if (!slot || !slot->deferred_active || info->si_code > 0 ||
      !(slot->deferred->promoted & (UINT64_C(1) << (number - 1)))) {
    return 0;
  }

  extern void signal_router_pending_observe(int) __attribute__((weak));
  if (signal_router_pending_observe) {
    signal_router_pending_observe(number);
  }
  int saved_errno = errno;
  uint64_t mask = signal_router_selection_begin();
  int guest_blocked =
      interrupted_tp != slot->host_tp &&
      (suspended_host_mask(slot) & (UINT64_C(1) << (number - 1)));
  int ignored = 0;
  uint64_t generation = signal_router_ignore_generation(number, &ignored);
  int discard = guest_blocked &&
                (ignored || generation != signal_router_token_ignore_generation(
                                              kernel_token));
  synchronize_deferred_at(slot, number, kernel_token);
  siginfo_t original;
  int target =
      gw_signal_deferred_take_original(slot->deferred, number, &original);
  int selected = 0;
  if (target > 0 && !discard) {
    if (guest_blocked) {
      if (signal_host_defer(slot, &original)) {
        signal_router_quarantine();
      }
    } else {
      selected = signal_router_select(target, &original, kernel_token);
    }
  }
  signal_router_selection_end(mask);

  if (target > 0 && !discard && !guest_blocked) {
    forward_host(target, &original, context, kernel_token, slot, interrupted_tp,
                 entry_mask, selected);
  }
  errno = saved_errno;
  return target >= 0;
}

static int dispatch_deferred(int number, siginfo_t *info, void *context,
                             struct signal_slot *slot, uintptr_t interrupted_tp,
                             uint64_t entry_mask, void *kernel_token) {
  if (dispatch_promoted(number, info, context, slot, interrupted_tp, entry_mask,
                        kernel_token)) {
    return 1;
  }
  const int *notifications = signal_router_fault_notifications();
  if (notifications) {
    for (int i = 0; i < GW_SIGNAL_FAULT_LANES; ++i) {
      if (number != notifications[i]) {
        continue;
      }
      if (!slot || !slot->deferred_active || interrupted_tp != slot->host_tp) {
        _exit(96);
      }
      extern void signal_router_pending_observe(int) __attribute__((weak));
      if (signal_router_pending_observe) {
        signal_router_pending_observe(number);
      }
      int saved_errno = errno;
      uint64_t selection_mask = signal_router_selection_begin();
      synchronize_deferred_at(slot, number, kernel_token);
      siginfo_t original;
      int target = gw_signal_deferred_take(slot->deferred, number, &original);
      void *token = target ? kernel_token : NULL;
      int selected =
          target ? signal_router_select(target, &original, token) : 0;
      signal_router_selection_end(selection_mask);
      if (target) {
        ucontext_t *frame = context;
        uint64_t mask;
        memcpy(&mask, &frame->uc_sigmask, sizeof mask);
        mask = signal_host_logical_mask(mask);
        memcpy(&frame->uc_sigmask, &mask, sizeof mask);
        forward_host(target, &original, context, token, slot, interrupted_tp,
                     entry_mask, selected);
        memcpy(&mask, &frame->uc_sigmask, sizeof mask);
        mask = signal_carrier_mask(slot, mask, 0);
        memcpy(&frame->uc_sigmask, &mask, sizeof mask);
      } else if (gw_signal_selection_hidden) {
        uint64_t unused;
        gw_signal_selection_hidden(context, &unused);
      }
      errno = saved_errno;
      return 1;
    }
  }
  if (slot && slot->deferred_active && interrupted_tp != slot->host_tp &&
      info->si_code <= 0 &&
      (gw_signal_fault_bits() & (UINT64_C(1) << (number - 1))) &&
      (suspended_host_mask(slot) & (UINT64_C(1) << (number - 1)))) {
    int saved_errno = errno;
    uint64_t mask = signal_router_selection_begin();
    int ignored = 0;
    uint64_t generation = signal_router_ignore_generation(number, &ignored);
    if (!ignored &&
        generation == signal_router_token_ignore_generation(kernel_token) &&
        signal_host_defer(slot, info)) {
      signal_router_quarantine();
    }
    signal_router_selection_end(mask);
    errno = saved_errno;
    return 1;
  }
  return 0;
}
#endif

/* Executed before the Host TP is restored. Must remain TLS/libc-free. */
__attribute__((no_stack_protector)) struct signal_slot *domain_signal_lookup(
    uintptr_t interrupted_tp) {
  long tid = gw_raw3(SYS_gettid, 0, 0, 0);
  struct signal_slot *dynamic = signal_dynamic_lookup(tid, interrupted_tp);
  if (dynamic) {
    return dynamic;
  }
  for (int i = 0; i < DOMAIN_FIXTURE_CARRIERS; ++i) {
    if (atomic_load_explicit(&signal_slots[i].tid, memory_order_acquire) ==
        tid) {
      return &signal_slots[i];
    }
  }
#ifdef GW_DOMAIN_CHAIN
  for (int i = 0; i <= DOMAIN_FIXTURE_CARRIERS; ++i) {
    if (atomic_load_explicit(&guest_tids[i], memory_order_acquire) == tid &&
        atomic_load_explicit(&guest_tps[i], memory_order_relaxed) ==
            interrupted_tp) {
      return (struct signal_slot *)(uintptr_t)-1;
    }
  }
#else
  (void)interrupted_tp;
#endif
  return NULL;
}

/* Host TLS and (on ARM64) x18 are valid here. Original kernel ucontext is
 * forwarded unchanged, except for the fixture's explicitly recognized fault. */
static void dispatch_core(int number, siginfo_t *info, void *context,
                          struct signal_slot *slot, uintptr_t interrupted_tp,
                          void *token, uint64_t entry_mask) {
#ifndef GW_DOMAIN_CHAIN
  (void)token;
  (void)interrupted_tp;
  (void)entry_mask;
#endif
#ifdef GW_DOMAIN_CHAIN
  if (signal_router_dispatch_internal(number, info, context, token)) {
#ifdef __GLIBC__
    if (number == GW_GNU_HOST_SETXID && slot &&
        interrupted_tp != slot->host_tp) {
      note_host_interruption(slot->exchange->guest_signal_target, 1, 1);
    }
#endif
    return;
  }
  if (dispatch_deferred(number, info, context, slot, interrupted_tp, entry_mask,
                        token)) {
    return;
  }
  if (slot && number == SIGUSR2 && interrupted_tp != slot->host_tp) {
    atomic_fetch_add_explicit(&domain_sr_guest_deliveries, 1,
                              memory_order_relaxed);
  }
  if (!slot) {
    forward_host(number, info, context, token, slot, interrupted_tp, entry_mask,
                 -1);
    return;
  }
#endif
  int saved_errno = errno;
  if (interrupted_tp != slot->host_tp) {
    save_guest_control(slot->exchange, context);
    if (slot->exchange->guest_signal && info->si_code > 0 &&
        (number == SIGSEGV || number == SIGBUS || number == SIGILL ||
         number == SIGFPE || number == SIGTRAP || number == SIGSYS)) {
      struct domain_signal_call call = {
          .entry = slot->exchange->guest_signal,
          .tp = interrupted_tp,
          .x18 = slot->exchange->interrupted_control[2],
          .control = {slot->exchange->interrupted_control[0],
                      slot->exchange->interrupted_control[1]},
          .number = number,
          .info = info,
          .context = context};
      if (domain_guest_signal_call(&call)) {
#ifdef GW_DOMAIN_CHAIN
        signal_router_default(number);
#else
        _exit(128 + number);
#endif
      }
      errno = saved_errno;
      return;
    }
  }
#ifdef GW_DOMAIN_TEST_SIGNALS
  atomic_fetch_add_explicit(&slot->failures, !adoption_signal_check(slot),
                            memory_order_relaxed);
#endif
  /* An asynchronous SIGSEGV is an ordinary Host-owned packet. Its signal
   * number alone does not identify a hardware fault in the Guest instruction.
   * https://github.com/torvalds/linux/blob/v6.12/kernel/signal.c
   */
  if (number == SIGSEGV && info->si_code > 0) {
    ucontext_t *uc = context;
#if defined(__x86_64__)
    uintptr_t pc = uc->uc_mcontext.gregs[REG_RIP];
#else
    uintptr_t pc = uc->uc_mcontext.pc;
#endif
    if (pc != (uintptr_t)domain_fault_instruction ||
        !slot->exchange->fault_page ||
        info->si_addr != slot->exchange->fault_page) {
#ifdef GW_DOMAIN_CHAIN
      /* An unrelated Guest fault is not a JVM implicit exception. Fail
       * closed instead of asking HotSpot to interpret a foreign stack. */
      if (interrupted_tp != slot->host_tp) {
        _exit(96);
      }
      forward_host(number, info, context, token, slot, interrupted_tp,
                   entry_mask, -1);
      errno = saved_errno;
      return;
#else
      gw_raw3(SYS_exit_group, 96, 0, 0);
      __builtin_unreachable();
#endif
    }
#if defined(__x86_64__)
    uc->uc_mcontext.gregs[REG_RIP] = (greg_t)domain_fault_resume;
#else
    uc->uc_mcontext.pc = (uintptr_t)domain_fault_resume;
#endif
    atomic_fetch_add_explicit(&slot->faults, 1, memory_order_relaxed);
  }
#ifdef GW_DOMAIN_TEST_SIGNALS
  else if (number == DOMAIN_NOTIFY_SIGNAL || number == DOMAIN_NESTED_SIGNAL) {
    atomic_fetch_add_explicit(&slot->signals, 1, memory_order_relaxed);
    if (number == DOMAIN_NOTIFY_SIGNAL) {
      /* DOMAIN_NESTED_SIGNAL is unmasked while this handler runs: nested Host
       * dispatch. */
      gw_raw3(SYS_tgkill, gw_raw3(SYS_getpid, 0, 0, 0),
              gw_raw3(SYS_gettid, 0, 0, 0), DOMAIN_NESTED_SIGNAL);
    }
  }
#endif
#ifdef GW_DOMAIN_CHAIN
  else {
    if (interrupted_tp != slot->host_tp && info->si_code > 0 &&
        (number == SIGBUS || number == SIGILL || number == SIGFPE)) {
      _exit(96);
    }
    if (interrupted_tp != slot->host_tp) {
      int index = number == SIGUSR2   ? 0
                  : number == SIGPIPE ? 1
                  : number == SIGXFSZ ? 2
                                      : -1;
      if (index >= 0) {
        ++slot->exchange->host_signal_delivered[index];
      }
      atomic_fetch_add_explicit(&domain_guest_host_signals, 1,
                                memory_order_relaxed);
    }
    forward_host(number, info, context, token, slot, interrupted_tp, entry_mask,
                 -1);
  }
#endif
  errno = saved_errno;
}

void domain_signal_dispatch(int number, siginfo_t *info, void *context,
                            struct signal_slot *slot, uintptr_t interrupted_tp,
                            void *token, uint64_t entry_mask) {
  if (slot && interrupted_tp != slot->host_tp) {
    restore_anchored_control(slot);
  }
  dispatch_core(number, info, context, slot, interrupted_tp, token, entry_mask);
}

void signal_install(void) {
  struct sigaction existing;
  const int owned[] = {DOMAIN_NOTIFY_SIGNAL, DOMAIN_NESTED_SIGNAL};
  for (unsigned i = 0; i < sizeof owned / sizeof owned[0]; ++i) {
    if (sigaction(owned[i], NULL, &existing) ||
        existing.sa_handler != SIG_DFL) {
      fprintf(stderr,
              "SIGNAL_SETUP occupied=%d handler=%p flags=0x%x errno=%d\n",
              owned[i], (void *)existing.sa_handler, existing.sa_flags, errno);
      _exit(97);
    }
  }
  struct sigaction action = {.sa_sigaction = domain_signal_entry,
                             .sa_flags = SA_SIGINFO | SA_ONSTACK};
  sigemptyset(&action.sa_mask);
  if (sigaction(DOMAIN_NOTIFY_SIGNAL, &action, NULL) ||
      sigaction(DOMAIN_NESTED_SIGNAL, &action, NULL)) {
    _exit(97);
  }
#ifndef GW_DOMAIN_CHAIN
  if (sigaction(SIGSEGV, &action, NULL)) {
    _exit(97);
  }
#endif
}

static int signal_try_register(struct signal_slot *slot, struct exchange *e) {
  slot->deferred_active = 0;
  slot->guest_async = 0;
  slot->deferred = NULL;
#if defined(__x86_64__)
  if (gw_raw3(SYS_arch_prctl, 0x1003, (long)&slot->host_tp, 0)) {
    return -1;
  }
#else
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(slot->host_tp));
  __asm__ volatile("mov %0, x18" : "=r"(slot->host_x18));
#endif
  slot->exchange = e;
  slot->signal_stack = mmap(NULL, 65536, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (slot->signal_stack == MAP_FAILED) {
    return -1;
  }
  stack_t alternate = {.ss_sp = slot->signal_stack, .ss_size = 65536};
  if (sigaltstack(&alternate, &slot->old_stack)) {
    munmap(slot->signal_stack, 65536);
    return -1;
  }
#ifdef GW_DOMAIN_CHAIN
  const int *notifications = signal_router_fault_notifications();
  if (notifications) {
    /* Keep the ordinary carrier small. Only the explicit deferred-fault trial
     * allocates a logical queue; inactive SDK contexts pay for one pointer.
     */
    slot->deferred = mmap(NULL, sizeof *slot->deferred, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (slot->deferred == MAP_FAILED) {
      slot->deferred = NULL;
      sigaltstack(&slot->old_stack, NULL);
      munmap(slot->signal_stack, 65536);
      return -1;
    }
    if (gw_signal_deferred_init(slot->deferred, notifications)) {
      munmap(slot->deferred, sizeof *slot->deferred);
      slot->deferred = NULL;
      sigaltstack(&slot->old_stack, NULL);
      munmap(slot->signal_stack, 65536);
      return -1;
    }

    uint64_t empty = 0;
    slot->consume_fd = syscall(SYS_signalfd4, -1, &empty, sizeof empty,
                               SFD_NONBLOCK | SFD_CLOEXEC);
    if (slot->consume_fd < 0) {
      int error = errno;
      munmap(slot->deferred, sizeof *slot->deferred);
      slot->deferred = NULL;
      sigaltstack(&slot->old_stack, NULL);
      munmap(slot->signal_stack, 65536);
      errno = error;
      return -1;
    }

    slot->deferred_active = 1;
    /* This empty queue starts in the current disposition generation. */
    memset(slot->deferred_ignore_generation, 0,
           sizeof slot->deferred_ignore_generation);
    synchronize_deferred(slot);
  }
#endif
  atomic_store_explicit(&slot->tid, e->host_tid, memory_order_release);
  sigset_t synthetic;
  sigemptyset(&synthetic);
#ifdef GW_DOMAIN_TEST_SIGNALS
  sigaddset(&synthetic, DOMAIN_NOTIFY_SIGNAL);
  sigaddset(&synthetic, DOMAIN_NESTED_SIGNAL);
#endif
  if (e->host_signal_probe) {
    /* The application exec path can inherit SIGPIPE blocked. Explicitly
     * enable delivery for this fixture, after registration, and restore the
     * entire inherited mask on exit. This is not Guest mask virtualization. */
    sigaddset(&synthetic, SIGUSR2);
    sigaddset(&synthetic, SIGPIPE);
    sigaddset(&synthetic, SIGXFSZ);
  }
  if (sigprocmask(SIG_UNBLOCK, &synthetic, &slot->inherited_mask)) {
    atomic_store_explicit(&slot->tid, 0, memory_order_release);
    sigaltstack(&slot->old_stack, NULL);
    munmap(slot->signal_stack, 65536);
    if (slot->deferred) {
      syscall(SYS_close, slot->consume_fd);
      munmap(slot->deferred, sizeof *slot->deferred);
      slot->deferred = NULL;
      slot->deferred_active = 0;
    }
    return -1;
  }
#ifdef GW_DOMAIN_TEST_SIGNALS
  slot->inherited_blocked =
      sigismember(&slot->inherited_mask, DOMAIN_NOTIFY_SIGNAL) ||
      sigismember(&slot->inherited_mask, DOMAIN_NESTED_SIGNAL);
#else
  slot->inherited_blocked = 0;
#endif
  if (slot->deferred_active) {
    atomic_fetch_add_explicit(&deferred_recipients, 1, memory_order_release);
  }
  return 0;
}

void signal_register(struct signal_slot *slot, struct exchange *e) {
  if (signal_try_register(slot, e)) {
    _exit(98);
  }
  publish_recipient(slot);
}

int signal_prepare_release(struct signal_slot *slot, int thread_exiting) {
#ifdef GW_DOMAIN_CHAIN
  if (slot && slot->deferred_active) {
    uint64_t mask = gw_nonlocal_mask_block();
    if (live_anchors && live_anchors->slot == slot) {
      gw_nonlocal_mask_restore(mask);
      return EBUSY;
    }
    synchronize_deferred(slot);
    gw_signal_deferred_prune(slot->deferred);
    if (thread_exiting) {
      uint64_t notifications = gw_signal_deferred_levels(slot->deferred);
      struct timespec zero = {0};
      siginfo_t information, original;
      int saved_errno = errno, number;
      while ((number = (int)syscall(SYS_rt_sigtimedwait, &notifications,
                                    &information, &zero,
                                    sizeof notifications)) > 0) {
        if (gw_signal_deferred_take_original(slot->deferred, number,
                                             &original) < 0) {
          gw_signal_deferred_take(slot->deferred, number, &original);
        }
      }
      int error = errno;
      errno = saved_errno;
      if (number != -1 || error != EAGAIN) {
        gw_nonlocal_mask_restore(mask);
        return error ? error : EIO;
      }
      if (!slot->deferred->armed) {
        uint64_t pending = gw_signal_pending_bits(&slot->deferred->pending);
        while (pending) {
          int original = __builtin_ctzll(pending) + 1;
          gw_signal_deferred_discard(slot->deferred, original);
          pending &= pending - 1;
        }
      }
    }
    int result = slot->deferred->armed ||
                         gw_signal_pending_bits(&slot->deferred->pending)
                     ? EBUSY
                     : 0;
    gw_nonlocal_mask_restore(mask);
    return result;
  }
#else
  (void)slot;
  (void)thread_exiting;
#endif
  return 0;
}

void signal_unregister(struct signal_slot *slot) {
#ifdef GW_DOMAIN_CHAIN
  if (slot->deferred_active) {
    /* This integration trial requires draining before teardown. Do not revoke
     * a recipient while a kernel notification can still name its old queue.
     * Retention beyond carrier teardown is a separate acceptance obligation.
     */
    uint64_t mask = gw_nonlocal_mask_block();
    if (gw_signal_deferred_close(slot->deferred)) {
      _exit(98);
    }
    syscall(SYS_close, slot->consume_fd);
    slot->consume_fd = -1;
    slot->deferred_active = 0;
    atomic_fetch_sub_explicit(&deferred_recipients, 1, memory_order_release);
    gw_nonlocal_mask_restore(mask);
  }
#endif
  /* Senders have quiesced. Block/drain only our synthetic asynchronous
   * signals before revoking the registry entry. Never mask hardware faults. */
  sigset_t synthetic;
  sigemptyset(&synthetic);
#ifdef GW_DOMAIN_TEST_SIGNALS
  sigaddset(&synthetic, DOMAIN_NOTIFY_SIGNAL);
  sigaddset(&synthetic, DOMAIN_NESTED_SIGNAL);
#endif
  if (sigprocmask(SIG_BLOCK, &synthetic, NULL)) {
    _exit(98);
  }
  struct timespec zero = {0};
  while (sigtimedwait(&synthetic, NULL, &zero) >= 0) {
  }
  atomic_store_explicit(&slot->tid, 0, memory_order_release);
  if (host_recipient == slot) {
    host_recipient = NULL;
  }
  if (slot->deferred) {
    munmap(slot->deferred, sizeof *slot->deferred);
    slot->deferred = NULL;
  }
  if (sigaltstack(&slot->old_stack, NULL)) {
    _exit(98);
  }
  munmap(slot->signal_stack, 65536);
#ifndef GW_DOMAIN_TEST_SIGNALS
  /* Production registration does not alter the Host mask. Do not roll back
   * legitimate changes made by the application during the carrier lifetime. */
  if (!slot->exchange->host_signal_probe) {
    return;
  }
#endif
  if (sigprocmask(SIG_SETMASK, &slot->inherited_mask, NULL)) {
    _exit(98);
  }
  sigset_t restored;
  if (sigprocmask(SIG_SETMASK, NULL, &restored)) {
    _exit(98);
  }
  /* Compare membership rather than padding in libc-specific sigset_t. */
  for (int number = 1; number < NSIG; ++number) {
    atomic_fetch_add_explicit(&slot->failures,
                              sigismember(&restored, number) !=
                                  sigismember(&slot->inherited_mask, number),
                              memory_order_relaxed);
  }
}

void signal_send(void) {
  long pid = syscall(SYS_getpid);
  for (int i = 0; i < DOMAIN_FIXTURE_CARRIERS; ++i) {
    long tid = atomic_load_explicit(&signal_slots[i].tid, memory_order_acquire);
    if (tid) {
      syscall(SYS_tgkill, pid, tid, DOMAIN_NOTIFY_SIGNAL);
    }
  }
}
