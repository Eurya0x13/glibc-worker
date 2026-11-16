#define _GNU_SOURCE
#include "signal-select.h"
#include "signal_channel.h"
#include "signal-router.h"
#include "signals.h"
#include "nonlocal-mask.h"
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
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
  *helper = notification(state->deferred, number);
  uint64_t higher = gw_signal_fault_bits() & (bit(number) | (bit(number) - 1));
  return (wanted & higher) | bit(*helper);
}

static int resolve_selection(struct gw_signal_selection *state, int helper,
                             int number, siginfo_t *information) {
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

  queue->armed &= ~bit(SIGSEGV);
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
  queue->armed &= ~bit(SIGSEGV);
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
  struct sleep_record *next;
  uint64_t return_mask;
  volatile sig_atomic_t interrupted, restart;
};

struct sleep_page {
  struct sleep_page *next;
  struct sleep_record records[32];
};

static _Thread_local struct sleep_page *pages;
static _Thread_local struct sleep_record *available;
static _Thread_local _Atomic(struct sleep_record *) waiting;
_Static_assert(ATOMIC_POINTER_LOCK_FREE == 2, "signal wait publication");
static pthread_key_t sleep_key;
static int sleep_key_error;
static pthread_once_t sleep_once = PTHREAD_ONCE_INIT;

static void destroy_sleep_records(void *argument) {
  (void)argument;
  uint64_t mask = gw_nonlocal_mask_block();
  int saved_errno = errno;
  waiting = NULL;
  available = NULL;
  while (pages) {
    struct sleep_page *next = pages->next;
    munmap(pages, sizeof *pages);
    pages = next;
  }
  gw_nonlocal_mask_restore(mask);
  errno = saved_errno;
}

static void prepare_key(void) {
  sleep_key_error = pthread_key_create(&sleep_key, destroy_sleep_records);
}

int signal_select_prepare(void) {
  pthread_once(&sleep_once, prepare_key);
  return sleep_key_error;
}

static struct sleep_record *sleep_record(void) {
  if (!available) {
    struct sleep_page *page = mmap(NULL, sizeof *page, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
      return NULL;
    }

    page->next = pages;
    pages = page;
    for (unsigned i = 0; i < 32; ++i) {
      page->records[i].next = available;
      available = &page->records[i];
    }
  }

  struct sleep_record *record = available;
  available = record->next;
  record->interrupted = record->restart = 0;
  return record;
}

int signal_select_recipient_prepare(void) {
  uint64_t mask = gw_nonlocal_mask_block();
  int error = pthread_setspecific(sleep_key, (void *)1);
  if (!error) {
    struct sleep_record *record = sleep_record();
    if (record) {
      record->next = available;
      available = record;
    } else {
      error = errno;
    }
  }
  gw_nonlocal_mask_restore(mask);
  return error;
}

void *gw_signal_selection_interrupt(void *token, void *context) {
  struct sleep_record *record =
      atomic_exchange_explicit(&waiting, NULL, memory_order_acq_rel);
  if (!record) {
    return NULL;
  }

  /* Detach before the user handler can enter another consumer or abandon the
   * interrupted native stack. Keep detached records until normal ppoll return
   * or thread exit, including saved signal-frame setcontext resumptions. */
  record->interrupted = 1;
  record->restart = signal_router_token_restart(token);
  ucontext_t *frame = context;
  memcpy(&frame->uc_sigmask, &record->return_mask, sizeof record->return_mask);
  return record;
}

int gw_signal_selection_sleep(int descriptor, uint64_t wanted,
                              const struct timespec *timeout, int restart) {
  uint64_t mask = gw_nonlocal_mask_block();
  struct sleep_record *record = sleep_record();
  if (!record) {
    gw_nonlocal_mask_restore(mask);
    return -1;
  }

  uint64_t physical = signal_host_wait_mask(wanted);
  if (syscall(SYS_signalfd4, descriptor, &physical, sizeof physical, 0) < 0) {
    int error = errno;
    record->next = available;
    available = record;
    gw_nonlocal_mask_restore(mask);
    errno = error;
    return -1;
  }

  record->return_mask = mask;
  waiting = record;
  struct pollfd poll = {.fd = descriptor, .events = POLLIN};
  int saved_errno = errno;
  struct timespec left;
  if (timeout) {
    left = *timeout;
  }
  int result = syscall(SYS_ppoll, &poll, 1, timeout ? &left : NULL, &mask, 8);
  int error = errno;
  uint64_t returned_mask = gw_nonlocal_mask_block();
  if (waiting == record) {
    waiting = NULL;
  }

  uint64_t empty = 0;
  if (signal_host_consumer_fd() == descriptor &&
      syscall(SYS_signalfd4, descriptor, &empty, sizeof empty, 0) < 0) {
    signal_router_quarantine();
    result = -1;
    error = EIO;
  }

  int interrupted = record->interrupted;
  int retry = restart && (!interrupted || record->restart);
  record->next = available;
  available = record;
  gw_nonlocal_mask_restore(interrupted ? returned_mask : mask);

  if (result < 0 && error == EINTR && retry) {
    errno = saved_errno;
    return 1;
  }
  errno = result < 0 ? error : saved_errno;
  return result;
}
