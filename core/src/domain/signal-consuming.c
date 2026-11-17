#define _GNU_SOURCE
#include "signal-consuming.h"
#include "nonlocal-mask.h"
#include "signal-user-copy.h"
#include <errno.h>
#include <limits.h>
#include <pthread.h>

static int failure(int error) {
  errno = error;
  return -1;
}

int gw_guest_sigpending(struct gw_signal_target *target, sigset_t *set) {
  int saved = errno;
  uint64_t mask = gw_nonlocal_mask_block();
  uint64_t pending = gw_signal_target_blocked_pending(target);
  int error = gw_signal_user_copy_out(set, &pending, sizeof pending);
  errno = error ? error : saved;
  gw_nonlocal_mask_restore(mask);
  return error ? -1 : 0;
}

static int wait_deadline(const struct timespec *timeout,
                         struct timespec *deadline) {
  struct timespec requested;
  int error = gw_signal_user_copy_in(&requested, timeout, sizeof requested);
  if (error) {
    return error;
  }
  if (requested.tv_sec < 0 || requested.tv_nsec < 0 ||
      requested.tv_nsec >= 1000000000) {
    return EINVAL;
  }
  if (clock_gettime(CLOCK_MONOTONIC, deadline)) {
    return errno;
  }

  long ns = deadline->tv_nsec + requested.tv_nsec;
  __int128 seconds =
      (__int128)deadline->tv_sec + requested.tv_sec + ns / 1000000000;
  *deadline = seconds > INT64_MAX
                  ? (struct timespec){INT64_MAX, 999999999}
                  : (struct timespec){(time_t)seconds, ns % 1000000000};
  return 0;
}

static int elapsed(const struct timespec *deadline) {
  if (!deadline) {
    return 0;
  }
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now)) {
    return -errno;
  }
  return now.tv_sec > deadline->tv_sec ||
         (now.tv_sec == deadline->tv_sec && now.tv_nsec >= deadline->tv_nsec);
}

static int copy_signal(siginfo_t *output, siginfo_t *packet, int number) {
  /* Apply GNU's public sigtimedwait normalization before kernel copyout.
   * https://github.com/bminor/glibc/blob/glibc-2.44/sysdeps/unix/sysv/linux/sigtimedwait.c
   */
  if (packet->si_code == SI_TKILL) {
    packet->si_code = SI_USER;
  }
  int error =
      output ? gw_signal_user_copy_out(output, packet, sizeof *packet) : 0;
  return error ? -error : number;
}

static int consume(struct gw_signal_target *target, uint64_t eligible,
                   siginfo_t *information, const struct timespec *deadline,
                   gw_libc_wait_event block) {
  uint64_t deliveries = 0;
  int first = 1, expired = 0;
  for (;;) {
    struct gw_signal_wait_sample sample;
    siginfo_t packet;
    uint64_t mask = gw_nonlocal_mask_block();
    int result = gw_signal_target_wait_take(target, eligible, &packet, &sample);
    if (result > 0) {
      result = copy_signal(information, &packet, result);
    } else if (!result && !first && sample.deliveries != deliveries) {
      result = -EINTR;
    }
    if (!result && first) {
      deliveries = sample.deliveries;
      first = 0;
    }
    gw_nonlocal_mask_restore(mask);
    if (result) {
      return result;
    }

    result = expired ? 1 : elapsed(deadline);
    if (result) {
      return result < 0 ? result : -EAGAIN;
    }
    result = block(&target->registry->wait_sequence, sample.sequence, deadline);
    if (result == -ETIMEDOUT) {
      expired = 1;
    } else if (result && result != -EAGAIN && result != -EINTR) {
      return result;
    }
    /* A matching packet wins over a concurrent timeout or interruption.
     * A stale futex wake or a Host-owned signal cannot create Guest EINTR.
     * https://github.com/torvalds/linux/blob/v6.12/kernel/signal.c */
  }
}

int gw_guest_sigtimedwait(struct gw_signal_target *target, const sigset_t *set,
                          siginfo_t *information,
                          const struct timespec *timeout,
                          gw_libc_wait_event block) {
  int saved = errno;
  pthread_testcancel();
  uint64_t eligible;
  int error = gw_signal_user_copy_in(&eligible, set, sizeof eligible);
  struct timespec deadline;
  if (!error && timeout) {
    error = wait_deadline(timeout, &deadline);
  }
  if (error) {
    return failure(error);
  }

  int result =
      consume(target, eligible, information, timeout ? &deadline : NULL, block);
  errno = result < 0 ? -result : saved;
  return result < 0 ? -1 : result;
}
