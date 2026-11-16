#define _GNU_SOURCE
#include "signal-consume.h"
#include "nonlocal.h"
#include "signals.h"
#include "signal-router.h"
#include <errno.h>
#include <pthread.h>

struct gw_signal_consumption {
  struct gw_signal_consumption *previous;
  struct gw_nonlocal_scope unwind;
  /* Modified after sigsetjmp, including from a routed signal handler. Keep
   * them volatile so the landing path can inspect their actual current state.
   */
  volatile siginfo_t packet[GW_SIGNAL_FAULT_LANES];
  volatile uint64_t seen, available;
  volatile sig_atomic_t native_active;
};

static _Thread_local struct gw_signal_consumption *consumer;
extern void gw_signal_consume_test_point(void) __attribute__((weak));

/* The kernel can dequeue before copying to the caller, and can deliver a
 * different signal before returning to the libc wrapper. Materialize ownership
 * before running any Host handler, including one that will abandon this call.
 * A cancelled level also needs a seen bit: decoding that old wake must never
 * steal a fresh signal queued by the intervening handler.
 * https://github.com/torvalds/linux/blob/v6.12/fs/signalfd.c
 * See signalfd_read_iter(): dequeue precedes signalfd_copyinfo(), even on
 * EFAULT.
 */
static void collect(struct gw_signal_consumption *state, int delivering) {
  if (!state->native_active) {
    return;
  }
  const int *notifications = signal_router_fault_notifications();
  if (!notifications) {
    _exit(125);
  }
  uint64_t pending;
  if (syscall(SYS_rt_sigpending, &pending, sizeof pending)) {
    _exit(125);
  }
  uint64_t unseen = ~state->seen;
  if (delivering > 0) {
    unseen &= ~(UINT64_C(1) << (delivering - 1));
  }
  for (int i = 0; i < GW_SIGNAL_FAULT_LANES; ++i) {
    uint64_t bit = UINT64_C(1) << (notifications[i] - 1);
    if (!(unseen & bit) || (pending & bit)) {
      continue;
    }
    siginfo_t information;
    int result =
        signal_host_collect_notification(notifications[i], &information);
    if (result >= 0) {
      state->seen |= bit;
      if (result) {
        state->packet[i] = information;
        state->available |= bit;
      }
    }
  }
}

int gw_signal_consume_busy(void) {
  return consumer != NULL;
}

struct gw_signal_consumption *gw_signal_consume_interrupt(int delivering) {
  struct gw_signal_consumption *state = consumer;
  if (state && state->native_active) {
    int saved_errno = errno;
    collect(state, delivering);
    state->native_active = 0;
    errno = saved_errno;
    return state;
  }
  return NULL;
}

void gw_signal_consume_resume(struct gw_signal_consumption *state) {
  if (state) {
    if (consumer != state) {
      _exit(125);
    }
    state->native_active = 1;
  }
}

int gw_signal_consume_notification(int number, siginfo_t *information) {
  const int *notifications = signal_router_fault_notifications();
  struct gw_signal_consumption *state = consumer;
  if (state && notifications) {
    for (int i = 0; i < GW_SIGNAL_FAULT_LANES; ++i) {
      if (notifications[i] != number) {
        continue;
      }
      uint64_t bit = UINT64_C(1) << (number - 1);
      if (state->seen & bit) {
        if (!(state->available & bit)) {
          return 0;
        }
        *information = state->packet[i];
        state->available &= ~bit;
        return information->si_signo;
      }
      break;
    }
  }
  return signal_host_take_notification(number, information);
}

static void retire(struct gw_signal_consumption *state) {
  if (consumer != state) {
    _exit(125);
  }
  collect(state, 0);
  state->native_active = 0;
  consumer = state->previous;
  /* Staged originals belong to an already consumed kernel wake. A native
   * jump/cancellation discards them, as it discards a native read's output.
   * A fresh queue/notification created by a handler is a separate ownership. */
}

static void cancelled(void *argument) {
  struct gw_signal_consumption *state = argument;
  uint64_t previous = gw_nonlocal_mask_block();
  retire(state);
  gw_nonlocal_leave(&state->unwind);
  gw_nonlocal_mask_restore(previous);
}

long gw_signal_consume_call(gw_signal_consume_fn invoke,
                            gw_signal_decode_fn decode, void *argument) {
  if (!signal_router_fault_notifications() || !gw_nonlocal_ready()) {
    long result = invoke(argument);
    uint64_t previous = gw_nonlocal_mask_block();
    result = decode(argument, result);
    gw_nonlocal_mask_restore(previous);
    return result;
  }
  uint64_t admission = gw_nonlocal_mask_block();
  struct gw_signal_consumption state = {.previous = consumer};
  volatile long result = -1;
  volatile int error = 0;
  volatile uint64_t returned_mask = admission;
  /* Capture inside the cleanup pair: unwinding to landing must leave libc's
   * cancellation record live until its normal unregister below. No lock or
   * allocation is carried across the native cancellation point. */
  pthread_cleanup_push(cancelled, &state);
  if (!sigsetjmp(state.unwind.landing, 0)) {
    gw_nonlocal_enter(&state.unwind);
    consumer = &state;
    state.native_active = 1;
    gw_nonlocal_mask_restore(admission);
    result = invoke(argument);
    error = errno;
    if (gw_signal_consume_test_point) {
      gw_signal_consume_test_point();
    }
    errno = error;
    returned_mask = gw_nonlocal_mask_block();
    collect(&state, 0);
    state.native_active = 0;
    result = decode(argument, result);
    error = errno;
  }
  retire(&state);
  pthread_cleanup_pop(0);
  gw_nonlocal_leave(&state.unwind); /* Continues an outstanding native jump. */
  gw_nonlocal_mask_restore(returned_mask);
  errno = error;
  return result;
}
