#define _GNU_SOURCE
#include "signal-deferred.h"
#include <errno.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

static const int faults[GW_SIGNAL_FAULT_LANES] = {SIGSEGV, SIGBUS,  SIGILL,
                                                  SIGFPE,  SIGTRAP, SIGSYS};

static uint64_t bit(int number) {
  return UINT64_C(1) << (number - 1);
}

uint64_t gw_signal_fault_bits(void) {
  uint64_t result = 0;
  for (int i = 0; i < GW_SIGNAL_FAULT_LANES; ++i) {
    result |= bit(faults[i]);
  }
  return result;
}

uint64_t gw_signal_notification_mask(const int *notifications,
                                     uint64_t logical) {
  for (int i = 0; i < GW_SIGNAL_FAULT_LANES; ++i) {
    logical &= ~bit(notifications[i]);
  }
  for (int i = 0; i < GW_SIGNAL_FAULT_LANES; ++i) {
    if (logical & bit(faults[i])) {
      logical |= bit(notifications[i]);
    }
  }
  return logical;
}

int gw_signal_deferred_level(const struct gw_signal_deferred *deferred,
                             int lane) {
  return deferred->promoted & bit(faults[lane]) ? faults[lane]
                                                : deferred->notification[lane];
}

uint64_t gw_signal_deferred_levels(const struct gw_signal_deferred *deferred) {
  uint64_t levels = 0;
  for (int i = 0; i < GW_SIGNAL_FAULT_LANES; ++i) {
    if (deferred->armed & bit(faults[i])) {
      levels |= bit(gw_signal_deferred_level(deferred, i));
    }
  }
  return levels;
}

int gw_signal_deferred_take_original(struct gw_signal_deferred *deferred,
                                     int number, siginfo_t *information) {
  if (number < 1 || number > 64 || !(deferred->promoted & bit(number))) {
    return -1;
  }

  deferred->promoted &= ~bit(number);
  deferred->armed &= ~bit(number);
  return gw_signal_pending_take(&deferred->pending, bit(number), information);
}

int gw_signal_deferred_promote(struct gw_signal_deferred *deferred,
                               uint64_t eligible) {
  struct timespec zero = {0};
  for (int i = 0; i < GW_SIGNAL_FAULT_LANES; ++i) {
    int original = faults[i];
    uint64_t target = bit(original);
    if (!(eligible & target) || (deferred->promoted & target)) {
      continue;
    }

    siginfo_t information;
    if (!gw_signal_pending_peek(&deferred->pending, target, &information)) {
      continue;
    }
    if (deferred->armed & target) {
      uint64_t wake = bit(deferred->notification[i]);
      siginfo_t unused;
      if (syscall(SYS_rt_sigtimedwait, &wake, &unused, &zero, 8) < 0) {
        return errno == EAGAIN ? EBUSY : errno;
      }
      deferred->armed &= ~target;
    }

    /* Original numbers restore kernel selection and restart decisions. A
     * concurrently queued newer standard packet may coalesce; the shadow
     * keeps the earlier metadata until the selected frame/dequeue binds it.
     * https://github.com/torvalds/linux/blob/v6.12/kernel/signal.c
     * See do_rt_tgsigqueueinfo(), legacy_queue() and dequeue_signal(). */
    if (syscall(SYS_rt_tgsigqueueinfo, deferred->process, deferred->recipient,
                original, &information)) {
      int error = errno;
      return gw_signal_deferred_arm(deferred) ? EIO : error;
    }
    deferred->armed |= target;
    deferred->promoted |= target;
  }
  return 0;
}

void gw_signal_deferred_prune(struct gw_signal_deferred *deferred) {
  uint64_t pending = gw_signal_pending_bits(&deferred->pending), cancelled = 0;
  for (int i = 0; i < GW_SIGNAL_FAULT_LANES; ++i) {
    if ((deferred->armed & bit(faults[i])) && !(pending & bit(faults[i]))) {
      cancelled |= bit(gw_signal_deferred_level(deferred, i));
    }
  }
  struct timespec zero = {0};
  siginfo_t information;
  int saved_errno = errno;
  while (cancelled) {
    int number =
        syscall(SYS_rt_sigtimedwait, &cancelled, &information, &zero, 8);
    if (number < 0) {
      break;
    }
    if (gw_signal_deferred_take_original(deferred, number, &information) < 0) {
      gw_signal_deferred_take(deferred, number, &information);
    }
  }
  errno = saved_errno;
}

static int original_lane(int number) {
  for (int i = 0; i < GW_SIGNAL_FAULT_LANES; ++i) {
    if (faults[i] == number) {
      return i;
    }
  }
  return -1;
}

int gw_signal_deferred_init(struct gw_signal_deferred *deferred,
                            const int *notifications) {
  uint64_t used = 0;
  for (int i = 0; i < GW_SIGNAL_FAULT_LANES; ++i) {
    int number = notifications[i];
    if (number < SIGRTMIN || number > SIGRTMAX || (used & bit(number))) {
      return EINVAL;
    }
    used |= bit(number);
  }
  memset(deferred, 0, sizeof *deferred);
  atomic_flag_clear_explicit(&deferred->pending.lock, memory_order_relaxed);
  deferred->process = getpid();
  deferred->recipient = (pid_t)syscall(SYS_gettid);
  memcpy(deferred->notification, notifications, sizeof deferred->notification);
  return 0;
}

int gw_signal_deferred_arm(struct gw_signal_deferred *deferred) {
  if (deferred->closed) {
    return ESRCH;
  }
  int saved_errno = errno, result = 0;
  uint64_t pending = gw_signal_pending_bits(&deferred->pending);
  for (int i = 0; i < GW_SIGNAL_FAULT_LANES; ++i) {
    uint64_t original = bit(faults[i]);
    if (!(pending & original) ||
        ((deferred->armed | deferred->promoted) & original)) {
      continue;
    }
    /* A level notification needs no sigqueue payload allocation guarantee.
     * Even if Linux falls back to a pending bit, original metadata survives.
     * https://github.com/torvalds/linux/blob/v6.12/kernel/signal.c
     */
    if (syscall(SYS_tgkill, deferred->process, deferred->recipient,
                deferred->notification[i])) {
      result = errno;
      break;
    }
    deferred->armed |= original;
  }
  errno = saved_errno;
  return result;
}

int gw_signal_deferred_put(struct gw_signal_deferred *deferred,
                           const siginfo_t *information) {
  if (deferred->closed) {
    return ESRCH;
  }
  if (original_lane(information->si_signo) < 0 || information->si_code > 0) {
    return EINVAL;
  }
  int result = gw_signal_pending_put(&deferred->pending, information, 0);
  return result ? result : gw_signal_deferred_arm(deferred);
}

int gw_signal_deferred_take(struct gw_signal_deferred *deferred,
                            int notification, siginfo_t *information) {
  for (int i = 0; i < GW_SIGNAL_FAULT_LANES; ++i) {
    if (deferred->notification[i] == notification) {
      uint64_t original = bit(faults[i]);
      deferred->armed &= ~original;
      return gw_signal_pending_take(&deferred->pending, original, information);
    }
  }
  return 0;
}

void gw_signal_deferred_discard(struct gw_signal_deferred *deferred,
                                int number) {
  if (original_lane(number) >= 0) {
    /* Do not clear armed: its notification is still kernel-pending. A fresh
     * original may reuse that level notification, but never its old siginfo.
     */
    gw_signal_pending_discard(&deferred->pending, number);
  }
}

int gw_signal_deferred_collect(struct gw_signal_deferred *deferred,
                               int notification, siginfo_t *information) {
  for (int i = 0; i < GW_SIGNAL_FAULT_LANES; ++i) {
    if (deferred->notification[i] == notification) {
      return (deferred->armed & bit(faults[i]))
                 ? gw_signal_deferred_take(deferred, notification, information)
                 : -1;
    }
  }
  return -1;
}

uint64_t gw_signal_deferred_notifications(
    const struct gw_signal_deferred *deferred) {
  uint64_t result = 0;
  for (int i = 0; i < GW_SIGNAL_FAULT_LANES; ++i) {
    result |= bit(deferred->notification[i]);
  }
  return result;
}

uint64_t gw_signal_deferred_mask(const struct gw_signal_deferred *deferred,
                                 uint64_t logical, int guest) {
  uint64_t physical = logical & ~gw_signal_deferred_notifications(deferred);
  for (int i = 0; i < GW_SIGNAL_FAULT_LANES; ++i) {
    if (guest || (logical & bit(faults[i])) ||
        (deferred->promoted & bit(faults[i]))) {
      physical |= bit(deferred->notification[i]);
    }
  }
  return physical;
}

uint64_t gw_signal_deferred_visible(const struct gw_signal_deferred *deferred,
                                    uint64_t kernel_pending) {
  return (kernel_pending & ~gw_signal_deferred_notifications(deferred)) |
         gw_signal_pending_bits(&deferred->pending);
}

int gw_signal_deferred_close(struct gw_signal_deferred *deferred) {
  if (deferred->armed || gw_signal_pending_bits(&deferred->pending)) {
    return EBUSY;
  }
  deferred->closed = 1;
  gw_signal_pending_close(&deferred->pending);
  gw_signal_pending_destroy(&deferred->pending);
  return 0;
}
