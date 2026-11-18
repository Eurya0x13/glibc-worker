#define _GNU_SOURCE
#include "signal-relay.h"
#include "signal-router.h"
#include "signal-stack.h"
#include "nonlocal-mask.h"
#include "raw_syscall.h"
#include <errno.h>
#include <linux/futex.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>

enum relay_state { STOPPED, STARTING, RUNNING, STOPPING, EXITING };

struct relay_request {
  struct relay_request *next;
  int number, selected;
  siginfo_t *information;
  void *context, *token;
  uint64_t mask;
  _Atomic int complete;
};

_Static_assert(ATOMIC_POINTER_LOCK_FREE == 2 && ATOMIC_INT_LOCK_FREE == 2,
               "foreign-TLS signal entry requires lock-free atomics");
static _Atomic(struct relay_request *) requests;
static _Atomic int state, notification;
#define RELAY_CLOSED (UINT_MAX - (UINT_MAX >> 1))
/* Admission and in-flight ownership share one modification order. A separate
 * running flag plus reader count would leave a stop/admission race. */
static _Atomic unsigned admission = RELAY_CLOSED;
static pthread_t dispatcher;

static __attribute__((no_stack_protector)) void wake(_Atomic int *word) {
  gw_raw3(SYS_futex, (long)word, FUTEX_WAKE_PRIVATE, 1);
}

static __attribute__((no_stack_protector)) void wait_value(_Atomic int *word,
                                                           int value) {
  /* Atomic load/recheck plus the kernel's compare-and-block closes the
   * publication/wait window. No polling, cancellation wrapper or Host errno.
   * https://github.com/torvalds/linux/blob/v6.12/kernel/futex/waitwake.c */
  while (atomic_load_explicit(word, memory_order_acquire) == value) {
    long result = gw_raw4(SYS_futex, (long)word, FUTEX_WAIT_PRIVATE, value, 0);
    if (result && result != -EAGAIN && result != -EINTR) {
      gw_raw3(SYS_exit_group, 125, 0, 0);
      __builtin_unreachable();
    }
  }
}

static struct relay_request *take_requests(void) {
  struct relay_request *batch =
      atomic_exchange_explicit(&requests, NULL, memory_order_acquire);
  struct relay_request *ordered = NULL;

  while (batch) {
    struct relay_request *next = batch->next;
    batch->next = ordered;
    ordered = batch;
    batch = next;
  }

  return ordered;
}

static void deliver(struct relay_request *request) {
  stack_t stack;
  gw_signal_stack_get(&stack);
  /* The dispatcher starts with no alternate stack. SA_ONSTACK consequently
   * falls back to its real pthread stack, as native delivery does. The source
   * kernel frame stays on its GNU recipient and must not become our stack. */
  request->selected = signal_router_select(
      request->number, request->information, request->token);
  signal_router_forward_selected_domain(
      request->number, request->information, request->context, request->token,
      request->mask, &stack, 0, 0, request->selected);

  _Atomic int *complete = &request->complete;
  atomic_store_explicit(complete, 1, memory_order_release);
  /* Nothing may dereference request after publication: its owner can return
   * from the signal frame as soon as it observes complete. A late futex wake
   * is harmless; neither user code nor the kernel retains this pointer. */
  wake(complete);
}

static void *dispatch(void *unused) {
  (void)unused;
  atomic_store_explicit(&state, RUNNING, memory_order_release);
  wake(&state);

  for (;;) {
    int observed = atomic_load_explicit(&notification, memory_order_acquire);
    struct relay_request *batch = take_requests();
    while (batch) {
      struct relay_request *next = batch->next;
      deliver(batch);
      batch = next;
    }

    if (atomic_load_explicit(&state, memory_order_acquire) == EXITING) {
      return NULL;
    }

    wait_value(&notification, observed);
  }
}

int signal_host_relay_running(void) {
  return atomic_load_explicit(&state, memory_order_acquire) == RUNNING;
}

int signal_host_relay_busy(void) {
  return (atomic_load_explicit(&admission, memory_order_acquire) &
          ~RELAY_CLOSED) != 0;
}

int signal_host_relay_start(void) {
  if (!signal_router_enabled()) {
    return EINVAL;
  }

  int expected = STOPPED;
  if (!atomic_compare_exchange_strong(&state, &expected, STARTING)) {
    return EALREADY;
  }

  uint64_t mask = gw_nonlocal_mask_block();
  int error = pthread_create(&dispatcher, NULL, dispatch, NULL);
  gw_nonlocal_mask_restore(mask);
  if (error) {
    atomic_store_explicit(&state, STOPPED, memory_order_release);
    return error;
  }

  wait_value(&state, STARTING);
  atomic_store_explicit(&admission, 0, memory_order_release);
  return 0;
}

int signal_host_relay_stop(void) {
  if (!signal_host_relay_running()) {
    return EINVAL;
  }
  if (pthread_equal(pthread_self(), dispatcher)) {
    return EDEADLK;
  }

  int expected = RUNNING;
  if (!atomic_compare_exchange_strong(&state, &expected, STOPPING)) {
    return EBUSY;
  }

  unsigned pending =
      atomic_fetch_or_explicit(&admission, RELAY_CLOSED, memory_order_acq_rel);
  if (pending & ~RELAY_CLOSED) {
    atomic_store_explicit(&state, RUNNING, memory_order_release);
    atomic_fetch_and_explicit(&admission, ~RELAY_CLOSED, memory_order_release);
    return EBUSY;
  }

  atomic_store_explicit(&state, EXITING, memory_order_release);
  atomic_fetch_add_explicit(&notification, 1, memory_order_release);
  wake(&notification);

  int error = pthread_join(dispatcher, NULL);
  if (error) {
    return error;
  }

  atomic_store_explicit(&state, STOPPED, memory_order_release);
  return 0;
}

__attribute__((no_stack_protector)) int signal_host_relay(
    int number, siginfo_t *information, void *context, void *token,
    uint64_t mask, int *selected) {
  unsigned observed = atomic_load_explicit(&admission, memory_order_relaxed);
  do {
    if (observed & RELAY_CLOSED) {
      return ENOTSUP;
    }
    if (observed == RELAY_CLOSED - 1) {
      return EOVERFLOW;
    }
  } while (!atomic_compare_exchange_weak_explicit(
      &admission, &observed, observed + 1, memory_order_acquire,
      memory_order_relaxed));

  struct relay_request request = {.number = number,
                                  .information = information,
                                  .context = context,
                                  .token = token,
                                  .mask = mask};
  struct relay_request *head =
      atomic_load_explicit(&requests, memory_order_relaxed);
  do {
    request.next = head;
  } while (!atomic_compare_exchange_weak_explicit(
      &requests, &head, &request, memory_order_release, memory_order_relaxed));

  atomic_fetch_add_explicit(&notification, 1, memory_order_release);
  wake(&notification);

  wait_value(&request.complete, 0);
  *selected = request.selected;
  atomic_fetch_sub_explicit(&admission, 1, memory_order_release);
  return 0;
}
