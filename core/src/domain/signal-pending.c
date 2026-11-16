#define _GNU_SOURCE
#include "signal-pending.h"
#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

enum { SIGNAL_CHUNK_NODES = 64 };

struct gw_signal_node {
  struct gw_signal_node *next;
  siginfo_t information;
};

struct gw_signal_chunk {
  struct gw_signal_chunk *next;
  struct gw_signal_node nodes[SIGNAL_CHUNK_NODES];
};

static void lock(struct gw_signal_pending *pending) {
  while (
      atomic_flag_test_and_set_explicit(&pending->lock, memory_order_acquire)) {
  }
}

static void unlock(struct gw_signal_pending *pending) {
  atomic_flag_clear_explicit(&pending->lock, memory_order_release);
}

static int valid_number(int number) {
  return number > 0 && number <= 64 && number != SIGKILL && number != SIGSTOP &&
         number != 32 && number != 33;
}

static uint64_t bit(int number) {
  return UINT64_C(1) << (number - 1);
}

static void attach_chunk(struct gw_signal_pending *pending,
                         struct gw_signal_chunk *chunk) {
  chunk->next = pending->chunks;
  pending->chunks = chunk;
  for (unsigned i = 0; i < SIGNAL_CHUNK_NODES; ++i) {
    chunk->nodes[i].next = pending->free;
    pending->free = &chunk->nodes[i];
  }
}

int gw_signal_pending_put(struct gw_signal_pending *pending,
                          const siginfo_t *information, uint64_t limit) {
  int number = information->si_signo;
  if (!valid_number(number)) {
    return EINVAL;
  }
  struct gw_signal_chunk *chunk = NULL;
  int result = 0, saved_errno = errno;
  for (;;) {
    lock(pending);
    if (chunk) {
      attach_chunk(pending, chunk);
      chunk = NULL;
    }
    if (pending->closed) {
      result = ESRCH;
    } else if (number < 32) {
      /* Linux standard signals coalesce, retaining the first siginfo rather
       * than replacing its sender/value on a later send.
       * https://github.com/torvalds/linux/blob/v6.12/kernel/signal.c */
      if (!(atomic_load_explicit(&pending->present, memory_order_relaxed) &
            bit(number))) {
        pending->standard[number] = *information;
        atomic_fetch_or_explicit(&pending->present, bit(number),
                                 memory_order_release);
      }
    } else if (pending->queued >= limit) {
      result = EAGAIN;
    } else if (pending->free) {
      struct gw_signal_node *node = pending->free;
      pending->free = node->next;
      node->next = NULL;
      node->information = *information;
      if (pending->last[number]) {
        pending->last[number]->next = node;
      } else {
        pending->first[number] = node;
      }
      pending->last[number] = node;
      ++pending->queued;
      atomic_fetch_or_explicit(&pending->present, bit(number),
                               memory_order_release);
    } else {
      unlock(pending);
      /* Allocate outside the lock. A concurrent producer may supply capacity
       * first; this chunk is still retained as reusable queue capacity. */
      chunk =
          (void *)syscall(SYS_mmap, NULL, sizeof *chunk, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      if (chunk == MAP_FAILED) {
        errno = saved_errno;
        return EAGAIN;
      }
      continue;
    }
    unlock(pending);
    errno = saved_errno;
    return result;
  }
}

int gw_signal_pending_take(struct gw_signal_pending *pending, uint64_t eligible,
                           siginfo_t *information) {
  lock(pending);
  uint64_t available =
      atomic_load_explicit(&pending->present, memory_order_relaxed) & eligible;
  if (!available || pending->closed) {
    unlock(pending);
    return 0;
  }
  int number = __builtin_ctzll(available) + 1;
  if (number < 32) {
    *information = pending->standard[number];
    atomic_fetch_and_explicit(&pending->present, ~bit(number),
                              memory_order_release);
  } else {
    struct gw_signal_node *node = pending->first[number];
    *information = node->information;
    pending->first[number] = node->next;
    if (!node->next) {
      pending->last[number] = NULL;
      atomic_fetch_and_explicit(&pending->present, ~bit(number),
                                memory_order_release);
    }
    node->next = pending->free;
    pending->free = node;
    --pending->queued;
  }
  unlock(pending);
  return number;
}

uint64_t gw_signal_pending_bits(const struct gw_signal_pending *pending) {
  return atomic_load_explicit(&pending->present, memory_order_acquire);
}

static void discard(struct gw_signal_pending *pending, int number) {
  if (number >= 32 && pending->first[number]) {
    struct gw_signal_node *node = pending->first[number];
    while (node) {
      --pending->queued;
      node = node->next;
    }
    pending->last[number]->next = pending->free;
    pending->free = pending->first[number];
    pending->first[number] = pending->last[number] = NULL;
  }
  atomic_fetch_and_explicit(&pending->present, ~bit(number),
                            memory_order_release);
}

void gw_signal_pending_discard(struct gw_signal_pending *pending, int number) {
  if (!valid_number(number)) {
    return;
  }
  lock(pending);
  discard(pending, number);
  unlock(pending);
}

void gw_signal_pending_close(struct gw_signal_pending *pending) {
  lock(pending);
  pending->closed = 1;
  for (int number = 1; number <= 64; ++number) {
    discard(pending, number);
  }
  unlock(pending);
}

void gw_signal_pending_destroy(struct gw_signal_pending *pending) {
  struct gw_signal_chunk *chunk = pending->chunks;
  while (chunk) {
    struct gw_signal_chunk *next = chunk->next;
    syscall(SYS_munmap, chunk, sizeof *chunk);
    chunk = next;
  }
  memset(pending, 0, sizeof *pending);
}
