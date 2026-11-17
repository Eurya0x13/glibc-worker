#ifndef GW_TEST_GUEST_ASYNC_H
#define GW_TEST_GUEST_ASYNC_H
#include "domain.h"
#include "signal-targets.h"
#include <linux/futex.h>
#include <stdatomic.h>
#include <sys/syscall.h>
#include <unistd.h>

enum {
  TEST_CARRIERS = 2,
  TEST_BATCHES = 2,
  TEST_PACKETS = 4,
  TEST_TOTAL_PACKETS = TEST_PACKETS + 3,
  TEST_LOGICAL_SIGNAL = 56,
  TEST_LAYOUT_WORDS = 12
};

enum { OWNER_ALLOC = 1, OWNER_SEND, OWNER_CLOSE, OWNER_FREE, OWNER_EMPTY };

enum {
  PHASE_READY = 1,
  PHASE_BUSY,
  PHASE_WAIT,
  PHASE_POLL,
  PHASE_GUEST,
  PHASE_DONE
};

struct async_carrier {
  struct exchange exchange;
  struct gw_signal_target *target;
  struct domain_signal_call entry;
  void *token, *entry_stack;
  _Atomic int phase, received, closed, guest_running;
  _Atomic int host_signal_count;
  _Atomic int empty_notifications;
  _Atomic int self_allowed;
  _Atomic int self_phase, self_received;
  _Atomic int self_errno;
  uintptr_t busy_begin, busy_end;
  int index, expected, failures;
  int host_handlers, guest_handlers, self_handlers, delay;
  int completed_work, budget_observed;
};

struct async_control {
  uint32_t layout[TEST_LAYOUT_WORDS];
  _Atomic int request, response, ready;
  struct async_carrier *carrier;
  int operation, error, value, notification;
  int send_lifecycle;
  void (*entry)(void *);
  struct gw_signal_targets targets;
};

static inline void test_layout(uint32_t words[TEST_LAYOUT_WORDS]) {
  const uint32_t expected[TEST_LAYOUT_WORDS] = {
      sizeof(struct async_control),
      sizeof(struct async_carrier),
      sizeof(struct exchange),
      sizeof(struct domain_signal_call),
      sizeof(struct gw_signal_target),
      sizeof(struct gw_signal_targets),
      sizeof(struct gw_signal_pending),
      sizeof(siginfo_t),
      offsetof(struct async_control, targets),
      offsetof(struct async_carrier, target),
      offsetof(struct async_carrier, entry),
      offsetof(struct async_carrier, received)};
  for (int i = 0; i < TEST_LAYOUT_WORDS; ++i) {
    words[i] = expected[i];
  }
}

static inline void wake(_Atomic int *word) {
  syscall(SYS_futex, word, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
}

static inline void wait_word(_Atomic int *word, int value) {
  syscall(SYS_futex, word, FUTEX_WAIT_PRIVATE, value, NULL, NULL, 0);
}

void domain_start(void);
#endif
