#define _GNU_SOURCE
#include "signal-quiescence.h"
#include "signal-router.h"
#include "../bootstrap/raw_syscall.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/futex.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

_Static_assert(ATOMIC_LONG_LOCK_FREE == 2 && ATOMIC_INT_LOCK_FREE == 2,
               "quiescence entry must work with either libc's TLS");

/* One process-owned collector. A timed-out packet can still reach its task;
 * its cookie is never reused and it carries no pointer into the snapshot. */
static uint64_t next_cookie;
static _Atomic uint64_t pending;
static _Atomic int recipient;
static _Atomic unsigned progress;
extern void gw_signal_quiescence_hide_wait(void *) __attribute__((weak));
#define ACK_PREFIX (UINT64_C(1) << 63)

__attribute__((no_stack_protector)) void gw_signal_quiescence_ack(
    int number, siginfo_t *info, void *context) {
  (void)number;
  if (info->si_code != SI_QUEUE ||
      info->si_pid != gw_raw3(SYS_getpid, 0, 0, 0)) {
    return;
  }

  /* Timed-out packets can arrive later and still interrupt a wrapped wait. */
  if (gw_signal_quiescence_hide_wait) {
    gw_signal_quiescence_hide_wait(context);
  }
  uint64_t cookie = (uintptr_t)info->si_value.sival_ptr;
  if (cookie &&
      atomic_load_explicit(&pending, memory_order_acquire) == cookie &&
      atomic_load_explicit(&recipient, memory_order_relaxed) ==
          gw_raw3(SYS_gettid, 0, 0, 0)) {
    uint64_t reply = signal_router_probe_prefix(context) ? ACK_PREFIX : 0;
    if (atomic_compare_exchange_strong_explicit(&pending, &cookie, reply,
                                                memory_order_release,
                                                memory_order_relaxed)) {
      atomic_fetch_add_explicit(&progress, 1, memory_order_release);
      gw_raw3(SYS_futex, (long)&progress, FUTEX_WAKE_PRIVATE, 1);
    }
  }
}

struct linux_dirent {
  uint64_t inode;
  int64_t offset;
  unsigned short length;
  unsigned char type;
  char name[];
};

struct task {
  int tid, stat_fd;
};

struct tasks {
  struct task *items;
  size_t count, capacity;
};

static int add_task(int directory, const char *name, struct tasks *tasks) {
  char *end;
  long tid = strtol(name, &end, 10);
  if (*end || tid <= 0 || tid > INT_MAX) {
    return EIO;
  }

  if (tasks->count == tasks->capacity) {
    if (tasks->capacity > SIZE_MAX / sizeof(*tasks->items) / 2) {
      return EOVERFLOW;
    }
    size_t capacity = tasks->capacity ? tasks->capacity * 2 : 32;
    struct task *items = realloc(tasks->items, capacity * sizeof *items);
    if (!items) {
      return ENOMEM;
    }
    tasks->items = items;
    tasks->capacity = capacity;
  }

  int dir = syscall(SYS_openat, directory, name,
                    O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
  if (dir < 0) {
    return errno == ENOENT || errno == ESRCH ? EBUSY : errno;
  }
  int fd = syscall(SYS_openat, dir, "stat", O_RDONLY | O_CLOEXEC, 0);
  int error = errno;
  syscall(SYS_close, dir);
  if (fd < 0) {
    return error == ENOENT || error == ESRCH ? EBUSY : error;
  }
  tasks->items[tasks->count++] = (struct task){.tid = tid, .stat_fd = fd};
  return 0;
}

static int read_tasks(int directory, struct tasks *tasks) {
  _Alignas(uint64_t) char bytes[8192];
  for (;;) {
    long size = syscall(SYS_getdents64, directory, bytes, sizeof bytes);
    if (size <= 0) {
      return size ? errno : 0;
    }

    for (size_t i = 0; i < (size_t)size;) {
      struct linux_dirent *entry = (void *)(bytes + i);
      size_t base = offsetof(struct linux_dirent, name);
      size_t left = size - i;
      if (left < base + 1 || entry->length < base + 1 || entry->length > left ||
          entry->length % _Alignof(struct linux_dirent) ||
          !memchr(entry->name, 0, entry->length - base)) {
        return EIO;
      }
      if (strcmp(entry->name, ".") && strcmp(entry->name, "..")) {
        int error = add_task(directory, entry->name, tasks);
        if (error) {
          return error;
        }
      }
      i += entry->length;
    }
  }
}

static int thread_count(int fd, long *count) {
  char data[2048];
  long size = syscall(SYS_pread64, fd, data, sizeof data - 1, 0);
  if (size <= 0) {
    return !size || errno == ESRCH || errno == ENOENT ? EBUSY : errno;
  }
  data[size] = 0;

  /* stat #2 is a parenthesized comm, which may itself contain spaces or ')'.
   * #20 is sampled under the same siglock as group membership changes.
   * A pinned proc stat inode refers to its original struct pid across reuse.
   * https://github.com/torvalds/linux/blob/v5.15/fs/proc/array.c
   * https://github.com/torvalds/linux/blob/v5.15/fs/proc/base.c
   * https://github.com/torvalds/linux/blob/v5.15/kernel/fork.c
   * https://github.com/torvalds/linux/blob/v5.15/kernel/exit.c */
  char *field = strrchr(data, ')');
  if (!field || field[1] != ' ') {
    return EIO;
  }
  field += 2;
  for (int index = 3; index < 20; ++index) {
    field = strchr(field, ' ');
    if (!field) {
      return EIO;
    }
    ++field;
  }
  char *end;
  long value = strtol(field, &end, 10);
  if (field == end || *end != ' ' || value < 0) {
    return EIO;
  }
  *count = value;
  return 0;
}

static int compare_tasks(const void *left, const void *right) {
  return ((const struct task *)left)->tid - ((const struct task *)right)->tid;
}

static int collect_tasks(struct tasks *tasks, int self) {
  int directory = syscall(SYS_openat, AT_FDCWD, "/proc/self/task",
                          O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
  if (directory < 0) {
    return errno;
  }
  int error = read_tasks(directory, tasks);
  syscall(SYS_close, directory);
  if (error || !tasks->count) {
    return error ? error : EBUSY;
  }

  qsort(tasks->items, tasks->count, sizeof *tasks->items, compare_tasks);
  int self_fd = -1;
  for (size_t i = 0; i < tasks->count; ++i) {
    const struct task *task = &tasks->items[i];
    if (i && task->tid == tasks->items[i - 1].tid) {
      return EBUSY;
    }
    if (task->tid == self) {
      self_fd = task->stat_fd;
    }
  }
  if (self_fd < 0) {
    return EBUSY;
  }

  /* getdents may skip threads during exit. All identities were pinned before
   * this kernel count. Each must still be in the group after the rendezvous;
   * equality then proves that no live member was missed at this boundary. */
  long count;
  error = thread_count(self_fd, &count);
  return error ? error : count == (long)tasks->count ? 0 : EBUSY;
}

static int acknowledge_task(int process, int number, const struct task *task,
                            const struct timespec *deadline) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now)) {
    return errno;
  }
  if (now.tv_sec > deadline->tv_sec ||
      (now.tv_sec == deadline->tv_sec && now.tv_nsec >= deadline->tv_nsec)) {
    return EBUSY;
  }
  if (next_cookie == ACK_PREFIX - 1) {
    return EOVERFLOW;
  }
  uint64_t cookie = ++next_cookie;
  siginfo_t info = {.si_signo = number, .si_code = SI_QUEUE};
  info.si_pid = process;
  info.si_uid = getuid();
  info.si_value.sival_ptr = (void *)(uintptr_t)cookie;
  atomic_store_explicit(&recipient, task->tid, memory_order_relaxed);
  atomic_store_explicit(&pending, cookie, memory_order_release);
  if (syscall(SYS_rt_tgsigqueueinfo, process, task->tid, number, &info)) {
    int error = errno;
    return error == ESRCH ? EBUSY : error;
  }

  for (;;) {
    unsigned observed = atomic_load_explicit(&progress, memory_order_acquire);
    uint64_t reply = atomic_load_explicit(&pending, memory_order_acquire);
    if (reply != cookie) {
      return reply == ACK_PREFIX ? -1 : 0;
    }
    if (syscall(SYS_futex, &progress, FUTEX_WAIT_BITSET_PRIVATE, observed,
                deadline, NULL, FUTEX_BITSET_MATCH_ANY) &&
        errno != EAGAIN && errno != EINTR) {
      return errno == ETIMEDOUT ? EBUSY : errno;
    }
  }
}

int gw_signal_quiescence_check(int number, unsigned timeout_ms) {
  struct timespec deadline;
  if (clock_gettime(CLOCK_MONOTONIC, &deadline)) {
    return errno;
  }
  deadline.tv_sec += timeout_ms / 1000;
  deadline.tv_nsec += (timeout_ms % 1000) * 1000000L;
  deadline.tv_sec += deadline.tv_nsec / 1000000000L;
  deadline.tv_nsec %= 1000000000L;

  int process = getpid(), self = syscall(SYS_gettid);
  struct tasks tasks = {0};
  int error = collect_tasks(&tasks, self);
  for (size_t i = 0; !error && i < tasks.count; ++i) {
    const struct task *task = &tasks.items[i];
    if (task->tid != self) {
      do {
        error = acknowledge_task(process, number, task, &deadline);
      } while (error == -1);
    }
    if (!error) {
      /* A numeric TID may now refer to a different thread. Reading through
       * the original inode, after acknowledgment, still needs a live member.
       * Success also validates membership at the earlier census boundary. */
      long count;
      error = thread_count(task->stat_fd, &count);
      if (!error && !count) {
        error = EBUSY;
      }
    }
  }

  atomic_store_explicit(&pending, 0, memory_order_release);
  for (size_t i = 0; i < tasks.count; ++i) {
    syscall(SYS_close, tasks.items[i].stat_fd);
  }
  free(tasks.items);
  return error;
}
