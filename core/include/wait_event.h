#ifndef GW_WAIT_EVENT_H
#define GW_WAIT_EVENT_H
#include <errno.h>
#include <limits.h>
#include <linux/futex.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include "rpc_metrics_internal.h"

#ifndef GW_FUTEX_WAKE_FILTER
#define GW_FUTEX_WAKE_FILTER 0
#endif
#if GW_FUTEX_WAKE_FILTER != 0 && GW_FUTEX_WAKE_FILTER != 1
#error GW_FUTEX_WAKE_FILTER must be 0 or 1
#endif
#ifndef GW_RPC_READY_POLL
#define GW_RPC_READY_POLL 0
#endif
#if GW_RPC_READY_POLL != 0 && GW_RPC_READY_POLL != 1
#error GW_RPC_READY_POLL must be 0 or 1
#endif
#ifndef GW_EVENT_TEST_HOOK
#define GW_EVENT_TEST_HOOK(point, event) ((void)0)
#endif

/* Only this 32-bit word crosses libc domains. No pthread object is shared.
 * The optional filter uses bit zero as a sticky waiter hint and the upper
 * 31 bits as the epoch. Only a notifier clears the hint; waiters never do. */
struct fb_event {
  _Atomic uint32_t sequence;
};

_Static_assert(sizeof(_Atomic uint32_t) == 4 && _Alignof(struct fb_event) >= 4,
               "Linux futex requires an aligned 32-bit atomic word");
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "Bridge events must be lock-free");

/* Acquire sample of the event word; the acquire pairs with the notifier's
 * release epoch advance, so a changed value also publishes the peer's
 * predicate data. Under the wake filter the waiter-hint bit is masked off:
 * callers always compare pure epochs. */
static inline uint32_t fb_event_sample(struct fb_event *event) {
  uint32_t value = atomic_load_explicit(&event->sequence, memory_order_acquire);
#if GW_FUTEX_WAKE_FILTER
  value &= ~UINT32_C(1);
#endif
  return value;
}

/* Release-advances the event word, then broadcasts FUTEX_WAKE_PRIVATE so
 * every waiter re-runs its predicate. With the filter enabled the epoch
 * still advances when no waiter hint is set - publication is never skipped,
 * only the WAKE syscall is. Preserves the caller's errno; an unexpected
 * futex failure is a runtime fault and aborts. */
static inline void fb_event_notify(struct fb_event *event) {
  int saved = errno;
  GW_RPC_SCOPE(cost, GW_RPC_NOTIFY, -1);

#if GW_FUTEX_WAKE_FILTER
  uint32_t previous =
      atomic_load_explicit(&event->sequence, memory_order_relaxed);
  while (!atomic_compare_exchange_weak_explicit(
      &event->sequence, &previous, (previous + 2) & ~UINT32_C(1),
      memory_order_release, memory_order_relaxed)) {
  }
  GW_EVENT_TEST_HOOK(4, event);
  if (!(previous & 1)) {
    goto done;
  }
#else
  atomic_fetch_add_explicit(&event->sequence, 1, memory_order_release);
#endif

  GW_RPC_COUNT(wake_syscalls, 1);
  long rc = syscall(SYS_futex, &event->sequence, FUTEX_WAKE_PRIVATE, INT_MAX,
                    NULL, NULL, 0);
  if (rc >= 0) {
    GW_RPC_COUNT(wake_threads, (uint64_t)rc);
  }
  if (rc < 0) {
    GW_RPC_COUNT(syscall_errors, 1);
    perror("bridge FUTEX_WAKE");
    abort();
  }

#if GW_RPC_PROFILE
  gw_rpc_notify_classify(&cost, rc);
#endif
#if GW_FUTEX_WAKE_FILTER
done:
#endif
  GW_RPC_SCOPE_END(cost);
  errno = saved;
}

/* Sample before checking/pumping the predicate, then wait with that sample.
   A notification between the check and syscall changes the word: EAGAIN.
   EINTR/spurious wakeups return to the caller's predicate loop. */
static inline void fb_event_wait(struct fb_event *event, uint32_t observed) {
  int saved = errno;
  GW_RPC_SCOPE(cost, GW_RPC_FUTEX_WAIT, -1);

#if GW_FUTEX_WAKE_FILTER
  uint32_t value = atomic_load_explicit(&event->sequence, memory_order_acquire);
  for (;;) {
    if ((value & ~UINT32_C(1)) != observed) {
      goto done;
    }
    if (value & 1) {
      break;
    }
    if (atomic_compare_exchange_weak_explicit(&event->sequence, &value,
                                              value | 1, memory_order_acq_rel,
                                              memory_order_acquire)) {
      break;
    }
  }
  GW_EVENT_TEST_HOOK(1, event);
  observed |= 1;
  GW_EVENT_TEST_HOOK(2, event);
#endif

  GW_RPC_COUNT(wait_syscalls, 1);
  long rc = syscall(SYS_futex, &event->sequence, FUTEX_WAIT_PRIVATE, observed,
                    NULL, NULL, 0);
  if (rc == 0) {
    GW_RPC_COUNT(wait_success, 1);
  } else if (errno == EAGAIN) {
    GW_RPC_COUNT(wait_eagain, 1);
  } else if (errno == EINTR) {
    GW_RPC_COUNT(wait_eintr, 1);
  } else {
    GW_RPC_COUNT(syscall_errors, 1);
  }
  if (rc < 0 && errno != EAGAIN && errno != EINTR) {
    perror("bridge FUTEX_WAIT");
    abort();
  }

#if GW_FUTEX_WAKE_FILTER
  GW_EVENT_TEST_HOOK(3, event);
done:
#endif
  GW_RPC_SCOPE_END(cost);
  errno = saved;
}

/* Only in-flight RPC waits use a short spin window. Idle workers/controllers
   always park immediately. Bound elapsed time, not CPU-dependent iterations. */
#if GW_RPC_READY_POLL
/* Ready-poll predicate: true when the epoch moved or this RPC's response
 * generation reached the expected value. The acquire load of response
 * pairs with the producer's release store, so a hit also means the
 * published payload is visible. */
static inline int fb_event_active_changed(struct fb_event *event,
                                          uint32_t observed,
                                          const _Atomic int *response,
                                          int expected) {
  return fb_event_sample(event) != observed ||
         (response &&
          atomic_load_explicit(response, memory_order_acquire) == expected);
}
/* Spin, then park. Parking goes through fb_event_wait with the caller's
 * ORIGINAL sample, so a notification that landed during the spin surfaces
 * as EAGAIN instead of a lost wakeup; the response flag is rechecked once
 * more before parking. Only the active RPC owner may pass a response
 * pointer; event-only callers use NULL. */
static inline void fb_event_wait_active_response(struct fb_event *event,
                                                 uint32_t observed,
                                                 const _Atomic int *response,
                                                 int expected) {
#else
static inline void fb_event_wait_active(struct fb_event *event,
                                        uint32_t observed) {
#endif
  GW_RPC_COUNT(active_waits, 1);
#if GW_RPC_READY_POLL
  if (fb_event_active_changed(event, observed, response, expected)) {
#else
  if (fb_event_sample(event) != observed) {
#endif
    GW_RPC_COUNT(ready_before_spin, 1);
    return;
  }

#if GW_RPC_SPIN_NS == 0
  GW_RPC_COUNT(spin_exhausted, 1);
  fb_event_wait(event, observed);
  return;
#else
  int saved = errno;
  GW_RPC_SCOPE(cost, GW_RPC_SPIN, -1);
  struct timespec start, now;
  if (clock_gettime(CLOCK_MONOTONIC, &start)) {
    perror("bridge monotonic clock");
    abort();
  }

  for (;;) {
    for (int i = 0; i < 32; i++) {
#if GW_RPC_READY_POLL
      if (fb_event_active_changed(event, observed, response, expected)) {
#else
      if (fb_event_sample(event) != observed) {
#endif
        GW_RPC_COUNT(spin_hits, 1);
#if GW_RPC_PROFILE
        uint64_t end = gw_rpc_clock(gw_rpc_current);
        uint64_t begin =
            (uint64_t)start.tv_sec * UINT64_C(1000000000) + start.tv_nsec;
        if (end != UINT64_MAX && end >= begin) {
          GW_RPC_COUNT(spin_ns, end - begin);
        }
#endif
        GW_RPC_SCOPE_END(cost);
        errno = saved;
        return;
      }
#if defined(__aarch64__)
      __asm__ volatile("yield" ::: "memory");
#elif defined(__x86_64__)
      __asm__ volatile("pause" ::: "memory");
#else
#error Unsupported bridge architecture
#endif
    }

    if (clock_gettime(CLOCK_MONOTONIC, &now)) {
      perror("bridge monotonic clock");
      abort();
    }
    int64_t elapsed =
        (int64_t)(now.tv_sec - start.tv_sec) * INT64_C(1000000000) +
        now.tv_nsec - start.tv_nsec;
    if (elapsed >= GW_RPC_SPIN_NS) {
      GW_RPC_COUNT(spin_ns, (uint64_t)elapsed);
      break;
    }
  }

  GW_RPC_COUNT(spin_exhausted, 1);
  GW_RPC_SCOPE_END(cost);
  errno = saved;
#if GW_RPC_READY_POLL
  if (response &&
      atomic_load_explicit(response, memory_order_acquire) == expected) {
    return;
  }
#endif

  fb_event_wait(event, observed);
#endif
}
#if GW_RPC_READY_POLL
/* Keep event-only callers and their race contracts unchanged. Only an active
 * RPC owner may additionally observe its matching response generation. */
static inline void fb_event_wait_active(struct fb_event *event,
                                        uint32_t observed) {
  fb_event_wait_active_response(event, observed, NULL, 0);
}
#endif
#endif
