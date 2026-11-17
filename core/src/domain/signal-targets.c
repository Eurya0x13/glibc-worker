#define _GNU_SOURCE
#include "signal-targets.h"
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <linux/futex.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#define TOMBSTONE ((struct gw_signal_target *)(uintptr_t)1)

static void lock(struct gw_signal_targets *registry) {
  while (atomic_flag_test_and_set_explicit(&registry->writer,
                                           memory_order_acquire)) {
  }
}

static void unlock(struct gw_signal_targets *registry) {
  atomic_flag_clear_explicit(&registry->writer, memory_order_release);
}

static int wake_waiters(struct gw_signal_targets *registry) {
  if (!registry->waits_enabled) {
    return 0;
  }
  atomic_fetch_add_explicit(&registry->wait_sequence, 1, memory_order_release);
  int saved = errno;
  int error = syscall(SYS_futex, &registry->wait_sequence, FUTEX_WAKE_PRIVATE,
                      INT_MAX, NULL, NULL, 0) < 0
                  ? errno
                  : 0;
  if (error && !registry->wake_error) {
    registry->wake_error = error;
  }
  errno = saved;
  return error;
}

static size_t hash(uintptr_t key) {
  key ^= key >> 30;
  key *= UINT64_C(0xbf58476d1ce4e5b9);
  key ^= key >> 27;
  key *= UINT64_C(0x94d049bb133111eb);
  return key ^ (key >> 31);
}

static struct gw_signal_target **find(struct gw_signal_targets *registry,
                                      uintptr_t key) {
  if (!registry->capacity) {
    return NULL;
  }
  size_t index = hash(key) & (registry->capacity - 1);
  struct gw_signal_target **empty = NULL;
  for (;;) {
    struct gw_signal_target **slot = &registry->slots[index];
    if (!*slot) {
      return empty ? empty : slot;
    }
    if (*slot == TOMBSTONE) {
      if (!empty) {
        empty = slot;
      }
    } else if ((*slot)->key == key) {
      return slot;
    }
    index = (index + 1) & (registry->capacity - 1);
  }
}

static int reserve_index(struct gw_signal_targets *registry) {
  if (registry->capacity &&
      registry->occupied < registry->capacity - registry->capacity / 4) {
    return 0;
  }
  size_t capacity = registry->capacity ? registry->capacity : 16;
  if (registry->count >= capacity / 2) {
    if (capacity > SIZE_MAX / 2) {
      return EOVERFLOW;
    }
    capacity *= 2;
  }
  if (capacity > SIZE_MAX / sizeof *registry->slots) {
    return EOVERFLOW;
  }
  struct gw_signal_target **slots =
      mmap(NULL, capacity * sizeof *slots, PROT_READ | PROT_WRITE,
           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (slots == MAP_FAILED) {
    return errno;
  }

  struct gw_signal_target **old = registry->slots;
  size_t old_capacity = registry->capacity;
  registry->slots = slots;
  registry->capacity = capacity;
  registry->occupied = registry->count;
  for (size_t i = 0; i < registry->count; ++i) {
    struct gw_signal_target *target = registry->active[i];
    *find(registry, target->key) = target;
  }
  if (old) {
    munmap(old, old_capacity * sizeof *old);
  }
  return 0;
}

static int reserve_active(struct gw_signal_targets *registry) {
  if (registry->count < registry->active_capacity) {
    return 0;
  }
  size_t capacity = registry->active_capacity;
  if (capacity > SIZE_MAX / 2 / sizeof *registry->active) {
    return EOVERFLOW;
  }
  capacity = capacity ? capacity * 2 : 16;
  struct gw_signal_target **active =
      mmap(NULL, capacity * sizeof *active, PROT_READ | PROT_WRITE,
           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (active == MAP_FAILED) {
    return errno;
  }
  if (registry->count) {
    memcpy(active, registry->active, registry->count * sizeof *active);
  }
  if (registry->active) {
    munmap(registry->active,
           registry->active_capacity * sizeof *registry->active);
  }
  registry->active = active;
  registry->active_capacity = capacity;
  return 0;
}

/* The inbox writer is held; active membership is stable under our outer
 * registry writer. Only recipients with an actually deliverable shared bit
 * are eligible. A blocked process packet remains pending until mask change. */
static int notify(void *argument, struct gw_signal_receiver *receiver) {
  struct gw_signal_targets *registry = argument;
  struct gw_signal_target *target = NULL;
  if (receiver) {
    target = (void *)((char *)receiver -
                      offsetof(struct gw_signal_target, receiver));
  } else {
    uint64_t pending = gw_signal_pending_bits(&registry->inbox.shared);
    for (size_t i = 0; i < registry->count; ++i) {
      if (registry->active[i]->tid > 0 &&
          (pending & ~registry->active[i]->receiver.blocked)) {
        target = registry->active[i];
        break;
      }
    }
  }
  int error = target && target->tid > 0 && registry->notify
                  ? registry->notify(registry->context, target)
                  : 0;
  if (error && !registry->wake_error) {
    registry->wake_error = error;
  }
  return error;
}

void gw_signal_targets_init(struct gw_signal_targets *registry,
                            uint64_t reserved, uint64_t limit,
                            gw_signal_target_notify callback, void *context) {
  memset(registry, 0, sizeof *registry);
  atomic_flag_clear(&registry->writer);
  atomic_init(&registry->wait_sequence, 0);
  gw_signal_inbox_init(&registry->inbox, reserved, limit);
  registry->notify = callback;
  registry->context = context;
}

int gw_signal_targets_enable_waits(struct gw_signal_targets *registry) {
  lock(registry);
  int error = registry->closed                        ? ESRCH
              : registry->waits_enabled               ? EALREADY
              : registry->count || registry->retained ? EBUSY
                                                      : 0;
  if (!error) {
    registry->waits_enabled = 1;
    error = wake_waiters(registry);
    if (error) {
      registry->waits_enabled = 0;
    }
  }
  unlock(registry);
  return error;
}

/* The outer writer spans allocation, publication and initial notification.
 * Do not implement open as prepare + activate: it is one admission operation.
 */
static int create_target(struct gw_signal_targets *registry, uintptr_t key,
                         int tid, uint64_t blocked, void *context,
                         struct gw_signal_target **output) {
  struct gw_signal_target **slot = find(registry, key);
  int error = registry->closed                      ? ESRCH
              : slot && *slot && *slot != TOMBSTONE ? EALREADY
              : registry->retained == SIZE_MAX      ? EOVERFLOW
                                                    : 0;
  if (!error) {
    error = reserve_index(registry);
  }
  if (!error) {
    error = reserve_active(registry);
  }
  struct gw_signal_target *target = MAP_FAILED;
  if (!error) {
    target = mmap(NULL, sizeof *target, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    error = target == MAP_FAILED ? errno : 0;
  }
  if (!error) {
    error =
        gw_signal_receiver_open(&registry->inbox, &target->receiver, blocked);
  }
  if (error) {
    if (target != MAP_FAILED) {
      munmap(target, sizeof *target);
    }
    return error;
  }

  target->registry = registry;
  target->key = key;
  target->tid = tid;
  target->pins = tid ? 0 : 1;
  target->context = context;
  target->index = registry->count;
  slot = find(registry, key);
  registry->occupied += !*slot;
  *slot = target;
  registry->active[registry->count++] = target;
  ++registry->retained;
  *output = target;
  /* A shared packet may predate this admission. Publish the caller's handle
   * before notifying it. Notification failure does not undo registration;
   * error()/retry() expose and repair accepted-but-unnotified work. */
  gw_signal_receiver_mask(&target->receiver, blocked, notify, registry);
  return 0;
}

int gw_signal_target_open(struct gw_signal_targets *registry, uintptr_t key,
                          int tid, uint64_t blocked, void *context,
                          struct gw_signal_target **output) {
  if (!key || tid <= 0 || !output) {
    return EINVAL;
  }

  lock(registry);
  int error = create_target(registry, key, tid, blocked, context, output);
  unlock(registry);
  return error;
}

int gw_signal_target_prepare(struct gw_signal_targets *registry, uintptr_t key,
                             uint64_t blocked, void *context,
                             struct gw_signal_target **output) {
  if (!key || !output) {
    return EINVAL;
  }

  lock(registry);
  int error = create_target(registry, key, 0, blocked, context, output);
  unlock(registry);
  return error;
}

int gw_signal_target_activate(struct gw_signal_target *target, int tid) {
  if (tid <= 0) {
    return EINVAL;
  }

  struct gw_signal_targets *registry = target->registry;
  lock(registry);
  int error = registry->closed || target->receiver.closed ? ESRCH
              : target->tid && target->tid != tid         ? EALREADY
                                                          : 0;
  if (!error && !target->tid) {
    target->tid = tid;
    gw_signal_receiver_mask(&target->receiver, target->receiver.blocked, notify,
                            registry);
  }
  unlock(registry);
  return error;
}

static int discard_pending(struct gw_signal_targets *registry, int number) {
  int error = gw_signal_inbox_discard(&registry->inbox, NULL, number);
  if (!error) {
    /* Closed identities already discarded their private queues. */
    for (size_t i = 0; i < registry->count; ++i) {
      error = gw_signal_inbox_discard(&registry->inbox,
                                      &registry->active[i]->receiver, number);
      if (error) {
        break;
      }
    }
  }
  return error;
}

static void cancel_opposite_pending(struct gw_signal_targets *registry,
                                    int number) {
  /* Linux prepare_signal cancels opposing pending job-control signals at
   * generation time, before checking masks or ignored dispositions. Kernel
   * process stop/resume effects still belong to the integration, not inboxes.
   * https://github.com/torvalds/linux/blob/v6.12/kernel/signal.c */
  if (number == SIGCONT) {
    discard_pending(registry, SIGTSTP);
    discard_pending(registry, SIGTTIN);
    discard_pending(registry, SIGTTOU);
  } else if (number == SIGTSTP || number == SIGTTIN || number == SIGTTOU) {
    discard_pending(registry, SIGCONT);
  }
}

static struct gw_signal_publication send_filtered(
    struct gw_signal_targets *registry, uintptr_t key, int tid,
    const siginfo_t *information, int ignored,
    struct gw_signal_target *representative, int dispositions) {
  if ((key && tid <= 0) || (!key && tid)) {
    return (struct gw_signal_publication){.status = EINVAL};
  }
  lock(registry);
  struct gw_signal_target **slot = key ? find(registry, key) : NULL;
  struct gw_signal_target *target = slot ? *slot : NULL;
  struct gw_signal_publication result = {0};
  if (registry->closed ||
      (key && (!target || target == TOMBSTONE || target->tid != tid))) {
    result.status = ESRCH;
  } else if (information->si_signo) {
    if (dispositions) {
      cancel_opposite_pending(registry, information->si_signo);
    }
    struct gw_signal_target *filter = target ? target : representative;
    if (!ignored || ((filter->receiver.blocked | filter->real_blocked) &
                     (UINT64_C(1) << (information->si_signo - 1)))) {
      result = gw_signal_inbox_put(&registry->inbox,
                                   target ? &target->receiver : NULL,
                                   information, notify, registry);
      if (!result.status) {
        int error = wake_waiters(registry);
        if (!result.wake_error) {
          result.wake_error = error;
        }
      }
    }
  }
  unlock(registry);
  return result;
}

struct gw_signal_publication gw_signal_targets_send(
    struct gw_signal_targets *registry, uintptr_t key, int tid,
    const siginfo_t *information) {
  return send_filtered(registry, key, tid, information, 0, NULL, 0);
}

struct gw_signal_publication gw_signal_targets_send_filtered(
    struct gw_signal_targets *registry, uintptr_t key, int tid,
    const siginfo_t *information, int ignored,
    struct gw_signal_target *representative) {
  if (!information || information->si_signo < 0 || information->si_signo > 64 ||
      (ignored && !key &&
       (!representative || representative->registry != registry))) {
    return (struct gw_signal_publication){.status = EINVAL};
  }
  return send_filtered(registry, key, tid, information, ignored, representative,
                       1);
}

struct gw_signal_publication gw_signal_target_mask(
    struct gw_signal_target *target, uint64_t mask) {
  return gw_signal_target_masks(target, mask, 0);
}

struct gw_signal_publication gw_signal_target_masks(
    struct gw_signal_target *target, uint64_t mask, uint64_t real) {
  struct gw_signal_targets *registry = target->registry;
  lock(registry);
  struct gw_signal_publication result = {0};
  if (registry->closed || target->receiver.closed) {
    result.status = ESRCH;
  } else {
    target->real_blocked = real;
    result.wake_error =
        gw_signal_receiver_mask(&target->receiver, mask, notify, registry);
  }
  unlock(registry);
  return result;
}

int gw_signal_target_deliver(struct gw_signal_target *target,
                             siginfo_t *information,
                             enum gw_signal_queue_source *source) {
  struct gw_signal_targets *registry = target->registry;
  lock(registry);
  int result = 0;
  *source = GW_SIGNAL_NONE;
  if (target->tid || target->receiver.closed) {
    result = gw_signal_inbox_deliver(&target->receiver, information, source);
  }
  unlock(registry);
  return result;
}

int gw_signal_target_take(struct gw_signal_target *target, uint64_t eligible,
                          siginfo_t *information,
                          enum gw_signal_queue_source *source) {
  struct gw_signal_targets *registry = target->registry;
  lock(registry);
  int result = 0;
  *source = GW_SIGNAL_NONE;
  if (target->tid || target->receiver.closed) {
    result =
        gw_signal_inbox_take(&target->receiver, eligible, information, source);
  }
  unlock(registry);
  return result;
}

int gw_signal_target_wait_take(struct gw_signal_target *target,
                               uint64_t eligible, siginfo_t *information,
                               struct gw_signal_wait_sample *sample) {
  struct gw_signal_targets *registry = target->registry;
  lock(registry);
  int result = -ENOTSUP;
  if (registry->waits_enabled) {
    if (registry->closed || target->receiver.closed || !target->tid) {
      result = -ESRCH;
    } else {
      enum gw_signal_queue_source source;
      result = gw_signal_inbox_take(&target->receiver, eligible, information,
                                    &source);
      sample->sequence =
          atomic_load_explicit(&registry->wait_sequence, memory_order_relaxed);
      sample->deliveries = target->deliveries;
    }
  }
  unlock(registry);
  return result;
}

void gw_signal_target_note_delivery(struct gw_signal_target *target) {
  struct gw_signal_targets *registry = target->registry;
  lock(registry);
  ++target->deliveries;
  wake_waiters(registry);
  unlock(registry);
}

uint64_t gw_signal_target_blocked_pending(struct gw_signal_target *target) {
  struct gw_signal_targets *registry = target->registry;
  lock(registry);
  uint64_t result =
      gw_signal_inbox_pending(&target->receiver) & target->receiver.blocked;
  unlock(registry);
  return result;
}

int gw_signal_targets_discard(struct gw_signal_targets *registry, int number) {
  lock(registry);
  int error = discard_pending(registry, number);
  unlock(registry);
  return error;
}

uint64_t gw_signal_target_pending(struct gw_signal_target *target) {
  struct gw_signal_targets *registry = target->registry;
  lock(registry);
  uint64_t result = gw_signal_inbox_pending(&target->receiver);
  unlock(registry);
  return result;
}

int gw_signal_target_pin(struct gw_signal_target *target) {
  struct gw_signal_targets *registry = target->registry;
  lock(registry);
  int error = target->receiver.closed || !target->tid ? ESRCH
              : target->pins == SIZE_MAX              ? EOVERFLOW
                                                      : 0;
  if (!error) {
    ++target->pins;
  }
  unlock(registry);
  return error;
}

int gw_signal_target_unpin(struct gw_signal_target *target) {
  struct gw_signal_targets *registry = target->registry;
  lock(registry);
  int error = target->pins ? 0 : EINVAL;
  if (!error) {
    --target->pins;
  }
  unlock(registry);
  return error;
}

int gw_signal_target_retain(struct gw_signal_target *target) {
  struct gw_signal_targets *registry = target->registry;
  lock(registry);
  int error = !target->pins ? EINVAL : target->pins == SIZE_MAX ? EOVERFLOW : 0;
  if (!error) {
    ++target->pins;
  }
  unlock(registry);
  return error;
}

static void close_target(struct gw_signal_target *target) {
  struct gw_signal_targets *registry = target->registry;
  if (target->receiver.closed) {
    return;
  }
  gw_signal_receiver_close(&target->receiver);
  *find(registry, target->key) = TOMBSTONE;
  struct gw_signal_target *last = registry->active[--registry->count];
  registry->active[target->index] = last;
  last->index = target->index;
}

void gw_signal_target_close(struct gw_signal_target *target) {
  struct gw_signal_targets *registry = target->registry;
  lock(registry);
  close_target(target);
  wake_waiters(registry);
  unlock(registry);
}

static int release(struct gw_signal_target *target) {
  struct gw_signal_targets *registry = target->registry;
  int error = !target->receiver.closed || target->pins ? EBUSY : 0;
  if (!error) {
    error = gw_signal_receiver_destroy(&target->receiver);
  }
  if (!error) {
    --registry->retained;
    munmap(target, sizeof *target);
  }
  return error;
}

int gw_signal_target_release(struct gw_signal_target *target) {
  struct gw_signal_targets *registry = target->registry;
  lock(registry);
  int error = release(target);
  unlock(registry);
  return error;
}

int gw_signal_target_retire(struct gw_signal_target *target) {
  struct gw_signal_targets *registry = target->registry;
  lock(registry);
  int error = !target->pins ? EINVAL : 0;
  if (!error && !--target->pins && target->receiver.closed) {
    error = release(target);
  }
  unlock(registry);
  return error;
}

int gw_signal_targets_retry(struct gw_signal_targets *registry) {
  lock(registry);
  int error = 0;
  for (size_t i = 0; i < registry->count; ++i) {
    struct gw_signal_target *target = registry->active[i];
    int next = gw_signal_receiver_mask(
        &target->receiver, target->receiver.blocked, notify, registry);
    if (next && !error) {
      error = next;
    }
  }
  registry->wake_error = error;
  unlock(registry);
  return error;
}

int gw_signal_targets_error(struct gw_signal_targets *registry) {
  lock(registry);
  int error = registry->wake_error;
  unlock(registry);
  return error;
}

int gw_signal_targets_only(struct gw_signal_targets *registry,
                           struct gw_signal_target *target) {
  lock(registry);
  int alone = registry->retained == 1 && registry->count == 1 &&
              registry->active[0] == target;
  unlock(registry);
  return alone;
}

void gw_signal_targets_close(struct gw_signal_targets *registry) {
  lock(registry);
  registry->closed = 1;
  while (registry->count) {
    close_target(registry->active[registry->count - 1]);
  }
  gw_signal_inbox_close(&registry->inbox);
  wake_waiters(registry);
  unlock(registry);
}

int gw_signal_targets_destroy(struct gw_signal_targets *registry) {
  lock(registry);
  int error = !registry->closed || registry->retained ? EBUSY : 0;
  if (!error) {
    error = gw_signal_inbox_destroy(&registry->inbox);
  }
  if (!error) {
    if (registry->slots) {
      munmap(registry->slots, registry->capacity * sizeof *registry->slots);
    }
    if (registry->active) {
      munmap(registry->active,
             registry->active_capacity * sizeof *registry->active);
    }
    registry->slots = registry->active = NULL;
    registry->capacity = registry->active_capacity = registry->occupied = 0;
  }
  unlock(registry);
  return error;
}
