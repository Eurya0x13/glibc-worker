#define _GNU_SOURCE
#include "signal-inbox.h"
#include <errno.h>
#include <limits.h>
#include <string.h>

static void lock(struct gw_signal_inbox *inbox) {
  while (
      atomic_flag_test_and_set_explicit(&inbox->lock, memory_order_acquire)) {
  }
}

static void unlock(struct gw_signal_inbox *inbox) {
  atomic_flag_clear_explicit(&inbox->lock, memory_order_release);
}

static uint64_t bit(int number) {
  return UINT64_C(1) << (number - 1);
}

static int allowed(int number) {
  return number > 0 && number <= 64 && number != SIGKILL && number != SIGSTOP &&
         number != 32 && number != 33;
}

void gw_signal_inbox_init(struct gw_signal_inbox *inbox, uint64_t reserved,
                          uint64_t limit) {
  memset(inbox, 0, sizeof *inbox);
  atomic_flag_clear_explicit(&inbox->lock, memory_order_relaxed);
  atomic_flag_clear_explicit(&inbox->shared.lock, memory_order_relaxed);
  inbox->reserved = reserved;
  inbox->limit = limit;
}

int gw_signal_receiver_open(struct gw_signal_inbox *inbox,
                            struct gw_signal_receiver *receiver,
                            uint64_t blocked) {
  lock(inbox);
  if (receiver->owner) {
    unlock(inbox);
    return EALREADY;
  }
  if (inbox->closed) {
    unlock(inbox);
    return ESRCH;
  }
  if (inbox->receivers == UINT_MAX) {
    unlock(inbox);
    return EOVERFLOW;
  }

  memset(receiver, 0, sizeof *receiver);
  atomic_flag_clear_explicit(&receiver->directed.lock, memory_order_relaxed);
  receiver->owner = inbox;
  receiver->blocked = blocked;
  ++inbox->receivers;
  unlock(inbox);
  return 0;
}

struct gw_signal_publication gw_signal_inbox_put(
    struct gw_signal_inbox *inbox, struct gw_signal_receiver *receiver,
    const siginfo_t *information, gw_signal_inbox_notify notify,
    void *argument) {
  int number = information->si_signo;
  if (!allowed(number) || information->si_code > 0 ||
      (receiver && receiver->owner != inbox)) {
    return (struct gw_signal_publication){.status = EINVAL};
  }
  if (inbox->reserved & bit(number)) {
    return (struct gw_signal_publication){.status = EBUSY};
  }

  lock(inbox);
  struct gw_signal_publication result = {0};
  if (inbox->closed || (receiver && receiver->closed)) {
    result.status = ESRCH;
  } else if (number >= 34 && inbox->queued >= inbox->limit) {
    result.status = EAGAIN;
  } else {
    struct gw_signal_pending *queue =
        receiver ? &receiver->directed : &inbox->shared;
    result.status = gw_signal_pending_put(queue, information, inbox->limit);
    if (!result.status) {
      inbox->queued += number >= 34;
      /* Publish before notifying, under the same transaction as recipient
       * closure. A self-notification cannot interrupt this locked sender;
       * another recipient waits until the complete packet is visible. */
      if (notify && (!receiver || !(receiver->blocked & bit(number)))) {
        result.wake_error = notify(argument, receiver);
      }
    }
  }

  unlock(inbox);
  return result;
}

int gw_signal_receiver_mask(struct gw_signal_receiver *receiver,
                            uint64_t blocked, gw_signal_inbox_notify notify,
                            void *argument) {
  struct gw_signal_inbox *inbox = receiver->owner;
  lock(inbox);
  if (inbox->closed || receiver->closed) {
    unlock(inbox);
    return ESRCH;
  }

  receiver->blocked = blocked;
  uint64_t available = gw_signal_pending_bits(&receiver->directed) |
                       gw_signal_pending_bits(&inbox->shared);
  int result = 0;
  if (notify && (available & ~blocked)) {
    result = notify(argument, receiver);
  }

  unlock(inbox);
  return result;
}

static int select_locked(struct gw_signal_receiver *receiver, uint64_t eligible,
                         siginfo_t *information,
                         enum gw_signal_queue_source *source) {
  struct gw_signal_inbox *inbox = receiver->owner;
  *source = GW_SIGNAL_NONE;
  int number = 0;
  if (!inbox->closed && !receiver->closed) {
    /* The wait/handler adapter supplies eligible explicitly. A consuming wait
     * may select logically blocked signals; a handler uses ~blocked instead.
     * Thread pending wins even over a higher-priority shared fault number.
     * https://github.com/torvalds/linux/blob/v6.12/kernel/signal.c
     * See dequeue_signal(), next_signal() and collect_signal(). */
    number = gw_signal_pending_take(&receiver->directed, eligible, information);
    if (number) {
      *source = GW_SIGNAL_THREAD;
    } else {
      number = gw_signal_pending_take(&inbox->shared, eligible, information);
      if (number) {
        *source = GW_SIGNAL_PROCESS;
      }
    }
    inbox->queued -= number >= 34;
  }

  return number;
}

int gw_signal_inbox_take(struct gw_signal_receiver *receiver, uint64_t eligible,
                         siginfo_t *information,
                         enum gw_signal_queue_source *source) {
  struct gw_signal_inbox *inbox = receiver->owner;
  lock(inbox);
  int number = select_locked(receiver, eligible, information, source);
  unlock(inbox);
  return number;
}

int gw_signal_inbox_deliver(struct gw_signal_receiver *receiver,
                            siginfo_t *information,
                            enum gw_signal_queue_source *source) {
  struct gw_signal_inbox *inbox = receiver->owner;
  lock(inbox);
  int number = select_locked(receiver, ~receiver->blocked, information, source);
  unlock(inbox);
  return number;
}

uint64_t gw_signal_inbox_pending(struct gw_signal_receiver *receiver) {
  struct gw_signal_inbox *inbox = receiver->owner;
  lock(inbox);
  uint64_t result = 0;
  if (!inbox->closed && !receiver->closed) {
    result = gw_signal_pending_bits(&receiver->directed) |
             gw_signal_pending_bits(&inbox->shared);
  }

  unlock(inbox);
  return result;
}

void gw_signal_receiver_close(struct gw_signal_receiver *receiver) {
  struct gw_signal_inbox *inbox = receiver->owner;
  lock(inbox);
  if (!receiver->closed) {
    inbox->queued -= receiver->directed.queued;
    gw_signal_pending_close(&receiver->directed);
    receiver->closed = 1;
    --inbox->receivers;
  }

  unlock(inbox);
}

void gw_signal_inbox_close(struct gw_signal_inbox *inbox) {
  lock(inbox);
  inbox->queued -= inbox->shared.queued;
  gw_signal_pending_close(&inbox->shared);
  inbox->closed = 1;
  unlock(inbox);
}

int gw_signal_receiver_destroy(struct gw_signal_receiver *receiver) {
  if (!receiver->closed) {
    return EBUSY;
  }

  gw_signal_pending_destroy(&receiver->directed);
  memset(receiver, 0, sizeof *receiver);
  return 0;
}

int gw_signal_inbox_destroy(struct gw_signal_inbox *inbox) {
  if (!inbox->closed || inbox->receivers) {
    return EBUSY;
  }

  gw_signal_pending_destroy(&inbox->shared);
  return 0;
}
