#define _GNU_SOURCE
#include "signal-select.h"
#include "signal_channel.h"
#include "signal-stack.h"
#include "signal-router.h"
#include "signals.h"
#include "nonlocal-mask.h"
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <time.h>
#include <ucontext.h>

extern void gw_signal_selection_test_point(void) __attribute__((weak));

int gw_host_signal_channel_enable(const int *notifications, size_t count) {
  _Static_assert((int)GW_HOST_FAULT_SIGNAL_COUNT == (int)GW_SIGNAL_FAULT_LANES,
                 "Host channel lane count");
  if (!notifications || count != GW_HOST_FAULT_SIGNAL_COUNT) {
    return EINVAL;
  }
  return signal_router_defer_faults(notifications);
}

static uint64_t bit(int number) {
  return UINT64_C(1) << (number - 1);
}

void gw_signal_selection_begin(struct gw_signal_selection *state) {
  uint64_t entry = gw_nonlocal_mask_block();
#ifndef __ANDROID__
  pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &state->cancellation);
#endif
  signal_router_selection_begin();
  state->mask = entry;
  state->deferred = signal_host_consumer();
  state->descriptor = signal_host_consumer_fd();
}

#ifndef __ANDROID__
static void restore_cancelled_mask(void *argument) {
  gw_nonlocal_mask_restore(*(uint64_t *)argument);
}
#endif

int gw_signal_selection_end(struct gw_signal_selection *state) {
  int saved_errno = errno;
  if (state->descriptor >= 0) {
    uint64_t empty = 0;
    if (syscall(SYS_signalfd4, state->descriptor, &empty, sizeof empty, 0) <
        0) {
      saved_errno = signal_router_quarantine();
    }
  }

  /* Release ownership while delivery remains blocked. Enabling a previously
   * asynchronous cancellation can unwind immediately: its bounded cleanup
   * restores the caller mask before external pthread cleanups run. No Host
   * handler can nonlocally abandon this cleanup record while it exists. */
  signal_router_selection_end(UINT64_MAX);
#ifndef __ANDROID__
  pthread_cleanup_push(restore_cancelled_mask, &state->mask);
  pthread_setcancelstate(state->cancellation, NULL);
  pthread_cleanup_pop(0);
#endif
  gw_nonlocal_mask_restore(state->mask);
  errno = saved_errno;
  return signal_router_error();
}

uint64_t gw_signal_selection_pending(struct gw_signal_selection *state,
                                     uint64_t wanted) {
  return state->deferred
             ? gw_signal_pending_bits(&state->deferred->pending) & wanted
             : 0;
}

static int notification(struct gw_signal_deferred *queue, int number) {
  static const int faults[] = {SIGSEGV, SIGBUS,  SIGILL,
                               SIGFPE,  SIGTRAP, SIGSYS};
  for (int i = 0; i < GW_SIGNAL_FAULT_LANES; ++i) {
    if (faults[i] == number) {
      return queue->notification[i];
    }
  }
  return 0;
}

/* Linux next_signal gives these six numbers priority, then chooses the
 * lowest number. dequeue_signal visits thread pending before shared pending.
 * Keep the winning helper in the kernel thread queue, excluding lower logical
 * priorities. Any higher native arrival still participates in kernel selection;
 * a higher shared arrival cannot overtake a thread-targeted deferred packet.
 * https://github.com/torvalds/linux/blob/v6.12/kernel/signal.c
 * See next_signal(), dequeue_signal() and collect_signal().
 */
static uint64_t candidates(struct gw_signal_selection *state, uint64_t wanted,
                           int *helper) {
  uint64_t shadow = gw_signal_selection_pending(state, wanted);
  *helper = 0;
  if (!shadow) {
    return signal_host_wait_mask(wanted);
  }

  int number = __builtin_ctzll(shadow) + 1;
  *helper = state->deferred->promoted & bit(number)
                ? number
                : notification(state->deferred, number);
  uint64_t higher = gw_signal_fault_bits() & (bit(number) | (bit(number) - 1));
  return (wanted & higher) | bit(*helper);
}

static int resolve_selection(struct gw_signal_selection *state, int helper,
                             int number, siginfo_t *information) {
  if (state->deferred) {
    int original =
        gw_signal_deferred_take_original(state->deferred, number, information);
    if (original >= 0) {
      return original;
    }
  }

  if (helper && number != helper && state->deferred &&
      gw_signal_selection_pending(state, bit(number))) {
    /* Native same-number thread input coalesces with the older deferred
     * packet. Its helper keeps this selection ahead of shared pending. Never
     * drain the original number again: post-dequeue sends are fresh instances.
     */
    uint64_t wake = bit(helper);
    struct timespec zero = {0};
    siginfo_t discarded;
    if (syscall(SYS_rt_sigtimedwait, &wake, &discarded, &zero, 8) != helper) {
      errno = signal_router_quarantine();
      return -2;
    }
    return gw_signal_deferred_take(state->deferred, helper, information);
  }

  return signal_host_take_notification(number, information);
}

int gw_signal_selection_take(struct gw_signal_selection *state, uint64_t wanted,
                             siginfo_t *information) {
  int error = signal_router_error();
  if (!error && state->deferred) {
    error = gw_signal_deferred_arm(state->deferred);
  }
  if (error) {
    errno = error;
    return -1;
  }

  int helper;
  uint64_t physical = candidates(state, wanted, &helper);
  struct timespec zero = {0};
  if (gw_signal_selection_test_point) {
    gw_signal_selection_test_point();
  }
  int number = syscall(SYS_rt_sigtimedwait, &physical, information, &zero, 8);
  if (number < 0) {
    return -1;
  }

  int original = resolve_selection(state, helper, number, information);
  return original == -2 ? -1 : original < 0 ? number : original;
}

int gw_signal_selection_record(struct gw_signal_selection *state,
                               uint64_t wanted, struct signalfd_siginfo *record,
                               siginfo_t *original) {
  int error = signal_router_error();
  if (!error && state->deferred) {
    error = gw_signal_deferred_arm(state->deferred);
  }
  if (error) {
    errno = error;
    return -1;
  }

  int helper;
  uint64_t physical = candidates(state, wanted, &helper);
  if (gw_signal_selection_mask(state, physical) < 0) {
    return -1;
  }
  if (gw_signal_selection_test_point) {
    gw_signal_selection_test_point();
  }
  if (syscall(SYS_read, state->descriptor, record, sizeof *record) < 0) {
    return -1;
  }

  int result = resolve_selection(state, helper, record->ssi_signo, original);
  if (!result || result == -2) {
    errno = result ? EIO : EAGAIN;
    return -1;
  }
  return result < 0 ? 0 : result;
}

int gw_signal_selection_mask(struct gw_signal_selection *state,
                             uint64_t wanted) {
  if (state->descriptor < 0) {
    errno = ENOTSUP;
    return -1;
  }
  return syscall(SYS_signalfd4, state->descriptor, &wanted, sizeof wanted, 0);
}

ssize_t gw_signal_selection_copy(struct gw_signal_selection *state,
                                 const struct iovec *vectors, int count) {
  /* Copy through a native signalfd, retaining the running kernel's readv
   * validation, partial vector and EFAULT behavior. The logical packet was
   * committed first. This temporary wake contains no user pointer or payload.
   * It is consumed/drained before unlocking, and an existing logical level is
   * rearmed before any handler can run.
   * https://github.com/torvalds/linux/blob/v6.12/fs/signalfd.c
   */
  struct gw_signal_deferred *queue = state->deferred;
  int helper = queue->notification[0];
  uint64_t wake = bit(helper);
  if (gw_signal_selection_mask(state, wake) < 0 ||
      syscall(SYS_tgkill, queue->process, queue->recipient, helper)) {
    return -1;
  }

  ssize_t result = syscall(SYS_readv, state->descriptor, vectors, count);
  int error = errno;
  struct timespec zero = {0};
  siginfo_t unused;
  while (syscall(SYS_rt_sigtimedwait, &wake, &unused, &zero, 8) > 0) {
  }

  if (!(queue->promoted & bit(SIGSEGV))) {
    queue->armed &= ~bit(SIGSEGV);
  }
  int rearmed = gw_signal_deferred_arm(queue);
  if (rearmed) {
    errno = rearmed;
    return -1;
  }
  errno = error;
  return result;
}

int gw_signal_selection_wait_copy(struct gw_signal_selection *state,
                                  siginfo_t *destination,
                                  const siginfo_t *source) {
  if (!destination) {
    return 0;
  }

  struct gw_signal_deferred *queue = state->deferred;
  int helper = queue->notification[0];
  uint64_t wake = bit(helper);
  if (syscall(SYS_tgkill, queue->process, queue->recipient, helper)) {
    return -1;
  }

  struct timespec zero = {0};
  int result = syscall(SYS_rt_sigtimedwait, &wake, destination, &zero, 8);
  int error = errno;
  siginfo_t unused;
  while (syscall(SYS_rt_sigtimedwait, &wake, &unused, &zero, 8) > 0) {
  }
  if (!(queue->promoted & bit(SIGSEGV))) {
    queue->armed &= ~bit(SIGSEGV);
  }
  int rearmed = gw_signal_deferred_arm(queue);
  if (rearmed) {
    errno = rearmed;
    return -1;
  }
  if (result < 0) {
    errno = error;
    return -1;
  }

  *destination = *source;
  return 0;
}

struct sleep_record {
  uintptr_t syscall_stack;
  struct sleep_record *next, *previous;
  uint64_t return_mask, execution_mask;
  uint64_t native_return_mask;
  int native_mask_valid, native_attempted;
  volatile sig_atomic_t interrupted, restart, hidden, retired, pinned;
  clockid_t native_clock;
  int native_clock_set, native_hidden_valid, native_visible_valid;
  int native_cancel_active, native_cancel_type, native_cancel_managed;
  int native_preentry_pending, native_host_depth, native_host_begin_valid;
  struct timespec native_host_begin;
  __int128 native_host_shift;
  struct timespec native_hidden_at, native_visible_at;
};

_Static_assert(offsetof(struct sleep_record, syscall_stack) == 0,
               "atomic wait assembly ABI");
_Static_assert(offsetof(struct sleep_record, execution_mask) == 32,
               "native wait assembly ABI");
_Static_assert(offsetof(struct sleep_record, native_return_mask) == 40,
               "native wait assembly ABI");
_Static_assert(offsetof(struct sleep_record, native_mask_valid) == 48,
               "native wait assembly ABI");
_Static_assert(offsetof(struct sleep_record, native_attempted) == 52,
               "native wait assembly ABI");
extern char gw_signal_wait_resume[];
extern char gw_signal_native_unmasked[], gw_signal_native_resume[];
extern char gw_signal_native_rearm[], gw_signal_native_rearm_blocked[];

int gw_signal_wait_result(long result) {
  if (result < 0 && result >= -4095) {
    errno = -result;
    return -1;
  }
  return result;
}

struct sleep_page {
  struct sleep_page *next;
  struct sleep_record records[32];
};

/* Pages and abandoned/saved rows stay pinned with the process runtime. Normal
 * returns recycle rows in O(1), including across Host threads without Guest
 * recipients. Raw mmap under a short masked lock avoids first-use pthread TLS
 * allocation from a signal handler. No automatic stack pointer is published.
 */
static struct sleep_page *sleep_pages;
static struct sleep_record *sleep_available;
static atomic_flag sleep_lock = ATOMIC_FLAG_INIT;
static _Thread_local _Atomic(struct sleep_record *) waiting;
_Static_assert(ATOMIC_POINTER_LOCK_FREE == 2, "signal wait publication");

void *gw_signal_wait_scope(void) {
  return atomic_load_explicit(&waiting, memory_order_acquire);
}

void gw_signal_wait_select(void *record) {
  atomic_store_explicit(&waiting, record, memory_order_release);
}

static void lock_sleep_records(void) {
  while (atomic_flag_test_and_set_explicit(&sleep_lock, memory_order_acquire)) {
  }
}

static void unlock_sleep_records(void) {
  atomic_flag_clear_explicit(&sleep_lock, memory_order_release);
}

int signal_select_prepare(void) {
  return 0;
}

static struct sleep_record *sleep_record(void) {
  lock_sleep_records();
  if (!sleep_available) {
    struct sleep_page *page =
        (void *)syscall(SYS_mmap, NULL, sizeof *page, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
      unlock_sleep_records();
      return NULL;
    }

    page->next = sleep_pages;
    sleep_pages = page;
    for (unsigned i = 0; i < 32; ++i) {
      page->records[i].next = sleep_available;
      sleep_available = &page->records[i];
    }
  }

  struct sleep_record *record = sleep_available;
  sleep_available = record->next;
  unlock_sleep_records();
  struct sleep_record *previous =
      atomic_load_explicit(&waiting, memory_order_acquire);
  while (previous && previous->retired) {
    previous = previous->previous;
  }
  if (previous) {
    previous->pinned = 1;
  }
  record->previous = previous;
  record->syscall_stack = 0;
  record->interrupted = record->restart = record->retired = record->pinned = 0;
  record->hidden = 0;
  record->native_mask_valid = record->native_attempted = 0;
  record->native_clock_set = record->native_hidden_valid = 0;
  record->native_visible_valid = 0;
  record->native_cancel_active = 0;
  record->native_cancel_managed = record->native_preentry_pending = 0;
  record->native_host_depth = record->native_host_begin_valid = 0;
  record->native_host_shift = 0;
  return record;
}

static void release_sleep_record(struct sleep_record *record) {
  record->retired = 1;
  if (record->pinned) {
    return;
  }
  lock_sleep_records();
  record->next = sleep_available;
  sleep_available = record;
  unlock_sleep_records();
}

static void *begin_mask_wait(uint64_t execution, int use_current) {
  uint64_t mask = gw_nonlocal_mask_block();
  if (use_current) {
    execution = mask;
  }
  execution = signal_host_physical_mask(execution);
  int error = signal_router_error();
  struct sleep_record *record = error ? NULL : sleep_record();
  if (!record) {
    error = error ? error : errno;
    gw_nonlocal_mask_restore(mask);
    errno = error;
    return NULL;
  }

  record->return_mask = mask;
  record->execution_mask = execution;
  atomic_store_explicit(&waiting, record, memory_order_release);
  return record;
}

void *gw_signal_mask_wait_begin(uint64_t execution) {
  return begin_mask_wait(execution, 0);
}

void *gw_signal_mask_wait_current_begin(void) {
  return begin_mask_wait(0, 1);
}

uint64_t gw_signal_mask_wait_execution(void *argument) {
  return ((struct sleep_record *)argument)->execution_mask;
}

int gw_signal_mask_wait_end(void *argument, int result, int error) {
  struct sleep_record *record = argument;
  uint64_t returned = gw_nonlocal_mask_block();
  struct sleep_record *expected = record;
  atomic_compare_exchange_strong_explicit(&waiting, &expected, record->previous,
                                          memory_order_acq_rel,
                                          memory_order_acquire);
  uint64_t mask = record->native_mask_valid ? record->native_return_mask
                  : record->interrupted     ? returned
                                            : record->return_mask;
  release_sleep_record(record);
  gw_nonlocal_mask_restore(mask);
  errno = error;
  return result;
}

#ifndef __ANDROID__
static void cancelled_mask_wait(void *record) {
  gw_signal_mask_wait_end(record, -1, ECANCELED);
}
#endif

static void cancellation_check(void *record, void (*probe)(void *),
                               void *argument) {
#ifndef __ANDROID__
  /* A cancellation before the mask-wait must not disappear between its last
   * cancellation check and syscall entry. Delivery stays blocked throughout
   * this short cleanup scope; a later SIGCANCEL stays kernel-pending until the
   * atomic wait installs its execution mask. The supplied native probe keeps
   * GNU FILE no-cancel reads non-cancellable. Interrupted raw waits return
   * EINTR and their caller checks cancellation after ordinary mask retirement.
   * No automatic cleanup pointer remains published while a user handler runs.
   * https://github.com/bminor/glibc/blob/glibc-2.41/nptl/pthread_cancel.c
   */
  pthread_cleanup_push(cancelled_mask_wait, record);
  probe(argument);
  pthread_cleanup_pop(0);
#else
  (void)record;
  probe(argument);
#endif
}

static void test_cancel(void *unused) {
  (void)unused;
#ifndef __ANDROID__
  pthread_testcancel();
#endif
}

void gw_signal_mask_wait_cancel_check(void *record) {
  cancellation_check(record, test_cancel, NULL);
}

enum wait_location { WAIT_OUTSIDE, WAIT_SYSCALL, WAIT_SETUP };

static inline __attribute__((always_inline)) enum wait_location locate_wait(
    const ucontext_t *frame) {
#if defined(__x86_64__)
  uintptr_t pc = frame->uc_mcontext.gregs[REG_RIP];
#else
  uintptr_t pc = frame->uc_mcontext.pc;
#endif
  if (pc == (uintptr_t)gw_signal_wait_resume ||
      pc == (uintptr_t)gw_signal_native_resume) {
    return WAIT_SYSCALL;
  }
  if ((pc >= (uintptr_t)gw_signal_native_unmasked &&
       pc < (uintptr_t)gw_signal_native_resume) ||
      (pc >= (uintptr_t)gw_signal_native_rearm &&
       pc < (uintptr_t)gw_signal_native_rearm_blocked)) {
    return WAIT_SETUP;
  }
  return WAIT_OUTSIDE;
}

static struct sleep_record *interrupted_wait(void *context, int hidden) {
  ucontext_t *frame = context;
  enum wait_location location = locate_wait(frame);
  if (location == WAIT_OUTSIDE) {
    return NULL;
  }

  uintptr_t stack = gw_signal_interrupted_sp(frame);
  struct sleep_record *record =
      atomic_load_explicit(&waiting, memory_order_acquire);
  while (record && (record->retired || record->syscall_stack != stack)) {
    record = record->previous;
  }
  if (location == WAIT_SETUP && !hidden) {
    /* A visible handler before native entry must leave an unstarted sleep
     * intact. It restores the caller's cancellation mode before user code;
     * rearm under a full mask before exposing the next syscall entry. */
    if (record && record->native_cancel_managed) {
      record->native_preentry_pending = 1;
#if defined(__x86_64__)
      frame->uc_mcontext.gregs[REG_RIP] = (uintptr_t)gw_signal_native_rearm;
#else
      frame->uc_mcontext.pc = (uintptr_t)gw_signal_native_rearm;
#endif
    }
    return NULL;
  }
  if (record && location == WAIT_SETUP) {
    /* A notification can arrive when rt_sigprocmask returns, before the
     * blocking syscall starts. Skip that not-yet-entered syscall, then let
     * the stable record's ordinary retry/visible-interruption path decide.
     * Otherwise an early wake could be followed by an indefinite sleep. */
    record->native_attempted = 0;
#if defined(__x86_64__)
    frame->uc_mcontext.gregs[REG_RIP] = (uintptr_t)gw_signal_native_resume;
    frame->uc_mcontext.gregs[REG_RAX] = -EINTR;
#else
    frame->uc_mcontext.pc = (uintptr_t)gw_signal_native_resume;
    frame->uc_mcontext.regs[0] = -EINTR;
#endif
  }
  return record;
}

int gw_signal_native_wait_attempted(void *argument) {
  return ((struct sleep_record *)argument)->native_attempted;
}

void gw_signal_native_wait_clock(void *argument, clockid_t clock) {
  struct sleep_record *record = argument;
  record->native_clock = clock;
  record->native_clock_set = 1;
}

int gw_signal_native_wait_interval(void *argument, struct timespec *begin,
                                   struct timespec *end) {
  struct sleep_record *record = argument;
  if (!record->hidden) {
    return ENOENT;
  }
  if (!record->native_hidden_valid) {
    return EIO;
  }
  *begin = record->native_hidden_at;
  if (record->interrupted) {
    if (!record->native_visible_valid) {
      return EIO;
    }
    *end = record->native_visible_at;
    return 0;
  }
  return clock_gettime(record->native_clock, end) ? errno : 0;
}

void gw_signal_native_wait_cancel_begin(void *argument) {
  struct sleep_record *record = argument;
  record->native_cancel_managed = 1;
#ifndef __ANDROID__
  record->native_cancel_active = 1;
  /* The owning libc publishes oldtype before acting on pending cancellation.
   * The caller has already installed its compiler unwind-only cleanup. No
   * pthread cleanup-buffer pointer is published into a signal-visible stack.
   * https://github.com/bminor/glibc/blob/glibc-2.41/nptl/pthread_setcanceltype.c
   */
  pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS,
                        &record->native_cancel_type);
#else
  (void)argument;
#endif
}

void gw_signal_native_wait_cancel_end(void *argument) {
#ifndef __ANDROID__
  struct sleep_record *record = argument;
  if (record->native_cancel_active) {
    record->native_cancel_active = 0;
    pthread_setcanceltype(record->native_cancel_type, NULL);
  }
#else
  (void)argument;
#endif
}

void gw_signal_native_wait_rearm(void *argument) {
  struct sleep_record *record = argument;
  record->return_mask = record->execution_mask;
  gw_signal_native_wait_cancel_begin(record);
}

__int128 gw_signal_native_wait_host_shift(void *argument) {
  struct sleep_record *record = argument;
  __int128 shift = record->native_host_shift;
  record->native_host_shift = 0;
  return shift;
}

static void note_visible(struct sleep_record *record) {
  if (record->hidden && !record->interrupted && record->native_clock_set) {
    record->native_visible_valid =
        !clock_gettime(record->native_clock, &record->native_visible_at);
  }
  record->interrupted = 1;
}

int gw_signal_selection_interrupt(void *token, void *context,
                                  uint64_t *execution) {
  struct sleep_record *record = interrupted_wait(context, 0);
  if (!record) {
    return 0;
  }
  gw_signal_native_wait_cancel_end(record);
  ucontext_t *frame = context;

  /* A kernel may construct several frames before the first user instruction.
   * Only the frame whose interrupted PC/SP belongs to this syscall owns its
   * saved return mask. An inner frame keeps its own kernel-saved mask.
   * Parent rows referenced by saved waits stay pinned, never recycled through
   * a nonlocal exit or a later stack resume. */
  atomic_store_explicit(&waiting, record->previous, memory_order_release);
  note_visible(record);
  record->restart = signal_router_token_restart(token);
  memcpy(&frame->uc_sigmask, &record->return_mask, sizeof record->return_mask);
  *execution = record->execution_mask;
  return 1;
}

int gw_signal_selection_hidden(void *context, uint64_t *execution) {
  struct sleep_record *record = interrupted_wait(context, 1);
  if (!record) {
    return 0;
  }
  gw_signal_native_wait_cancel_end(record);

  /* Keep a hidden wait published until the retry decision. A real Host
   * handler may run after this frame, or during future Guest handler work;
   * that visible delivery must prevent transparent EINTR retry. Nested waits
   * retain their own rows and restore this row when they retire. */
  record->hidden = 1;
  if (record->native_clock_set) {
    record->native_hidden_valid =
        !clock_gettime(record->native_clock, &record->native_hidden_at);
  }
  ucontext_t *frame = context;
  memcpy(&frame->uc_sigmask, &record->return_mask, sizeof record->return_mask);
  *execution = record->execution_mask;
  extern void gw_signal_hidden_wait_test_point(void) __attribute__((weak));
  if (gw_signal_hidden_wait_test_point) {
    gw_signal_hidden_wait_test_point();
  }
  return 1;
}

__attribute__((no_stack_protector)) void gw_signal_quiescence_hide_wait(
    void *context) {
  /* The assembly gateway executes on Host TLS. Check its PC before reading
   * any TLS, because the private rendezvous also reaches real GNU threads. */
  if (locate_wait(context) == WAIT_OUTSIDE) {
    return;
  }
  int saved = errno;
  uint64_t execution;
  gw_signal_selection_hidden(context, &execution);
  errno = saved;
}

void gw_signal_wait_host_delivery(int restart) {
  struct sleep_record *record =
      atomic_load_explicit(&waiting, memory_order_acquire);
  for (; record; record = record->previous) {
    gw_signal_native_wait_cancel_end(record);
    if (!record->retired && record->native_preentry_pending &&
        record->native_clock_set && !record->native_host_depth++) {
      record->native_host_begin_valid =
          !clock_gettime(record->native_clock, &record->native_host_begin);
    }
    if (!record->retired && record->hidden) {
      record->restart = restart && (!record->interrupted || record->restart);
      note_visible(record);
    }
  }
}

void gw_signal_wait_host_return(void) {
  struct sleep_record *record =
      atomic_load_explicit(&waiting, memory_order_acquire);
  for (; record; record = record->previous) {
    if (record->retired || !record->native_host_depth ||
        --record->native_host_depth) {
      continue;
    }
    struct timespec now;
    if (record->native_host_begin_valid &&
        !clock_gettime(record->native_clock, &now)) {
      record->native_host_shift +=
          ((__int128)now.tv_sec - record->native_host_begin.tv_sec) *
              1000000000 +
          now.tv_nsec - record->native_host_begin.tv_nsec;
    }
    record->native_preentry_pending = 0;
  }
}

static int retry_wait(void *argument, int result, int error, int native,
                      int restart, int cancellable) {
  struct sleep_record *record = argument;
  if (result >= 0 || error != EINTR ||
      (!record->hidden && !(restart && record->restart))) {
    return 0;
  }

  uint64_t returned = gw_nonlocal_mask_block();
  if (record->interrupted && !(restart && record->restart)) {
    gw_nonlocal_mask_restore(returned);
    return 0;
  }
  record->hidden = 0;
  record->interrupted = record->restart = 0;
  record->native_mask_valid = 0;
  record->native_hidden_valid = record->native_visible_valid = 0;
  record->syscall_stack = 0;
  atomic_store_explicit(&waiting, record, memory_order_release);
  if (native && cancellable) {
    test_cancel(NULL); /* The caller owns a compiler unwind-only cleanup. */
  } else if (!native) {
    gw_signal_mask_wait_cancel_check(record);
  }
  return 1;
}

int gw_signal_mask_wait_retry(void *argument, int result, int error) {
  return retry_wait(argument, result, error, 0, 0, 1);
}

int gw_signal_native_wait_retry(void *argument, int result, int error) {
  return retry_wait(argument, result, error, 1, 0, 1);
}

int gw_signal_native_read_retry(void *argument, int result, int error,
                                int cancellable) {
  return retry_wait(argument, result, error, 1, 1, cancellable);
}

int gw_signal_selection_sleep(int descriptor, uint64_t wanted,
                              const struct timespec *timeout, int restart,
                              void (*probe)(void *), void *argument) {
  uint64_t mask = gw_nonlocal_mask_block();
  struct sleep_record *record = sleep_record();
  if (!record) {
    gw_nonlocal_mask_restore(mask);
    return -1;
  }

  uint64_t physical = signal_host_wait_mask(wanted);
  if (syscall(SYS_signalfd4, descriptor, &physical, sizeof physical, 0) < 0) {
    int error = errno;
    release_sleep_record(record);
    gw_nonlocal_mask_restore(mask);
    errno = error;
    return -1;
  }

  record->return_mask = mask;
  record->execution_mask = mask;
  waiting = record;
  cancellation_check(record, probe, argument);
  struct pollfd poll = {.fd = descriptor, .events = POLLIN};
  int saved_errno = errno;
  struct timespec left;
  if (timeout) {
    left = *timeout;
  }
  extern void gw_signal_selection_sleep_test_point(void) __attribute__((weak));
  if (gw_signal_selection_sleep_test_point) {
    gw_signal_selection_sleep_test_point();
  }
  int result = gw_signal_wait_result(gw_signal_wait_syscall(
      record, SYS_ppoll, (long)&poll, 1, (long)(timeout ? &left : NULL),
      (long)&mask, sizeof mask, 0));
  int error = errno;
  uint64_t returned_mask = gw_nonlocal_mask_block();
  if (waiting == record) {
    waiting = record->previous;
  }

  uint64_t empty = 0;
  if (signal_host_consumer_fd() == descriptor &&
      syscall(SYS_signalfd4, descriptor, &empty, sizeof empty, 0) < 0) {
    signal_router_quarantine();
    result = -1;
    error = EIO;
  }

  int interrupted = record->interrupted;
  int retry = (!interrupted && record->hidden) ||
              (restart && (!interrupted || record->restart));
  release_sleep_record(record);
  gw_nonlocal_mask_restore(interrupted ? returned_mask : mask);

  if (result < 0 && error == EINTR && retry) {
    errno = saved_errno;
    return 1;
  }
  errno = result < 0 ? error : saved_errno;
  return result;
}
