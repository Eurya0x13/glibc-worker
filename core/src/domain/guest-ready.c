#define _GNU_SOURCE
#include "guest-ready.h"
#include "guest-epoll.h"
#include "guest-fd-lease.h"
#include "guest-signalfd.h"
#include "nonlocal-mask.h"
#include "signal-user-copy.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

#define NS_PER_S 1000000000L
#define WORD_BITS (sizeof(unsigned long) * CHAR_BIT)

struct wait_clock {
  struct timespec deadline;
  int timed;
};

struct descriptor_wait {
  struct gw_signal_target *target;
  gw_libc_poll_wait poll;
  struct gw_guest_fd_lease *lease;
  struct wait_clock clock;
  struct gw_signal_poll_sample sample;
  uint64_t physical, kernel;
  int wake_fd, active, interrupted;
};

struct fd_sets {
  fd_set *user[3];
  unsigned long *result[3];
  size_t words, descriptors;
};

struct poll_table {
  struct pollfd *user, *kernel;
  size_t count;
  struct fd_sets sets;
};

static int finish(int status, int saved) {
  errno = status < 0 ? -status : saved;
  return status < 0 ? -1 : status;
}

int gw_guest_descriptor(struct gw_signal_target *target,
                        enum gw_libc_descriptor_op op, int fd, int new_fd,
                        int flags, int *handled) {
  int saved = errno;
  if (op == GW_DESCRIPTOR_CLOSE) {
    pthread_testcancel();
  }

  int changed = op == GW_DESCRIPTOR_CLOSE ? fd : new_fd;
  uint64_t physical = gw_nonlocal_mask_block();
  gw_guest_fds_lock();
  *handled = gw_guest_signalfd_cookie(changed) ||
             gw_guest_epoll_known(changed) ||
             (op != GW_DESCRIPTOR_CLOSE && gw_guest_signalfd_cookie(fd));
  int status = 0;
  if (*handled) {
    if (op == GW_DESCRIPTOR_CLOSE) {
      status = syscall(SYS_close, fd);
    } else if (op == GW_DESCRIPTOR_DUP2 && fd == new_fd) {
      status = syscall(SYS_fcntl, fd, F_GETFL, 0) < 0 ? -1 : fd;
    } else {
      status = syscall(SYS_dup3, fd, new_fd, flags);
    }
    if (status < 0) {
      status = -errno;
    }
    gw_guest_signalfd_reap();
    gw_guest_epoll_reap();
    gw_signal_targets_wake_waiters(target->registry);
  }
  gw_guest_fds_unlock();
  gw_nonlocal_mask_restore(physical);
  return finish(status, saved);
}

static int start_clock(struct wait_clock *time, const struct timespec *limit) {
  if (!limit) {
    return 0;
  }
  if (limit->tv_sec < 0 || limit->tv_nsec < 0 || limit->tv_nsec >= NS_PER_S) {
    return -EINVAL;
  }

  if (clock_gettime(CLOCK_MONOTONIC, &time->deadline)) {
    return -errno;
  }
  time->timed = 1;
  long ns = time->deadline.tv_nsec + limit->tv_nsec;
  time_t carry = ns >= NS_PER_S;
  if (limit->tv_sec > INT64_MAX - time->deadline.tv_sec - carry) {
    time->deadline = (struct timespec){INT64_MAX, NS_PER_S - 1};
  } else {
    time->deadline.tv_sec += limit->tv_sec + carry;
    time->deadline.tv_nsec = ns - carry * NS_PER_S;
  }
  return 0;
}

static int remaining(struct wait_clock *time, struct timespec *left) {
  if (!time->timed) {
    return 1;
  }

  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now)) {
    return -errno;
  }

  time_t seconds = time->deadline.tv_sec - now.tv_sec;
  long nanos = time->deadline.tv_nsec - now.tv_nsec;
  if (nanos < 0) {
    --seconds;
    nanos += NS_PER_S;
  }
  if (seconds < 0) {
    *left = (struct timespec){0};
    return 0;
  }
  *left = (struct timespec){seconds, nanos};
  return seconds || nanos;
}

static int begin(struct descriptor_wait *wait, const struct timespec *timeout,
                 const sigset_t *mask) {
  int status = start_clock(&wait->clock, timeout);
  if (status) {
    return status;
  }
  pthread_testcancel();
  wait->physical = gw_nonlocal_mask_block();
  wait->active = 1;
  wait->kernel = wait->physical;
  if (mask) {
    memcpy(&wait->kernel, mask, sizeof wait->kernel);
  }
  return 0;
}

static void end(struct descriptor_wait *wait) {
  if (wait->active) {
    int saved = errno;
    wait->active = 0;
    gw_signal_target_poll_disarm(wait->target);
    gw_guest_fd_lease_drop(&wait->lease);
    errno = saved;
    gw_nonlocal_mask_restore(wait->physical);
  }
}

static void expose_ready_mask(struct descriptor_wait *wait,
                              const sigset_t *mask, int status) {
  if (mask && status >= 0) {
    /* The mask-and-wait syscalls install their temporary mask even when a
     * descriptor was already ready. Allow a pending handler to run with that
     * mask before libc restores the caller state. The ready result is fixed. */
    struct timespec zero = {0};
    syscall(SYS_ppoll, NULL, 0, &zero, &wait->kernel, 8);
  }
}

static int sample(struct descriptor_wait *wait, int arm) {
  return -gw_signal_target_poll_arm(wait->target, &wait->sample,
                                    arm ? &wait->wake_fd : NULL);
}

static int by_descriptor(const void *left, const void *right) {
  const struct pollfd *a = left, *b = right;
  return (a->fd > b->fd) - (a->fd < b->fd);
}

static size_t wait_count(struct pollfd *fds, size_t count) {
  struct rlimit limit;
  if (getrlimit(RLIMIT_NOFILE, &limit) || count <= limit.rlim_cur) {
    return count;
  }

  /* Kernel ppoll counts all entries, even duplicates. Coalesce only the
   * private blocking set so app watches still return individual revents. */
  qsort(fds, count, sizeof *fds, by_descriptor);
  size_t used = 1;
  for (size_t i = 1; i < count; ++i) {
    if (fds[used - 1].fd == fds[i].fd) {
      fds[used - 1].events |= fds[i].events;
    } else {
      fds[used++] = fds[i];
    }
  }
  return used;
}

static int sleep_until_change(struct descriptor_wait *wait, struct pollfd *fds,
                              size_t count) {
  count = wait_count(fds, count);
  struct timespec left;
  int time_left = remaining(&wait->clock, &left);
  if (time_left <= 0) {
    return time_left;
  }

  int result =
      wait->poll(fds, count, wait->clock.timed ? &left : NULL, &wait->kernel);
  int error = result < 0 ? errno : 0;
  if (error && error != EINTR) {
    return -error;
  }

  /* The eventfd can wake the same syscall as an actual Guest handler. Keep
   * that interruption after the next readiness scan. Private notifications
   * for packets another waiter already consumed leave neither result set. */
  struct gw_signal_poll_sample after;
  int status = gw_signal_target_poll_arm(wait->target, &after, NULL);
  if (status) {
    return -status;
  }
  wait->interrupted =
      after.deliveries != wait->sample.deliveries ||
      (error && after.notifications == wait->sample.notifications);
  return 0;
}

static int kernel_poll(struct pollfd *fds, size_t count) {
  struct timespec zero = {0};
  int status = syscall(SYS_ppoll, fds, count, &zero, NULL, 8);
  return status < 0 ? -errno : status;
}

static void reap(void) {
  gw_guest_signalfd_reap();
  gw_guest_epoll_reap();
}

static int poll_plan(struct poll_table *table, struct descriptor_wait *wait) {
  int managed = 0;
  for (size_t i = 0; i < table->count; ++i) {
    struct pollfd *user = &table->user[i];
    struct pollfd *kernel = &table->kernel[i];
    *kernel = *user;
    user->revents = 0;
    uint64_t cookie = gw_guest_signalfd_cookie(user->fd);
    if (cookie) {
      uint64_t mask;
      gw_guest_signalfd_mask(cookie, &mask);
      user->revents = wait->sample.pending & mask ? user->events & POLLIN : 0;
      kernel->fd = -1;
      managed = 1;
    } else if ((user->events & (POLLIN | POLLRDNORM)) &&
               gw_guest_epoll_valid(user->fd) == 1) {
      if (gw_guest_epoll_ready(user->fd, &wait->sample)) {
        user->revents = user->events & (POLLIN | POLLRDNORM);
      }
      managed = 1;
    }
  }
  return managed;
}

static int select_result(struct poll_table *table) {
  /* Linux select counts ready bits across the three sets, including multiple
   * sets for the same descriptor, and treats any invalid descriptor as EBADF.
   * https://github.com/torvalds/linux/blob/v6.12/fs/select.c
   */
  static const short visible[] = {
      POLLIN | POLLRDNORM | POLLRDBAND | POLLHUP | POLLERR,
      POLLOUT | POLLWRNORM | POLLWRBAND | POLLERR, POLLPRI};
  int ready = 0;
  for (size_t j = 0; j < 3; ++j) {
    if (table->sets.result[j]) {
      memset(table->sets.result[j], 0,
             table->sets.words * sizeof(unsigned long));
    }
  }

  for (size_t i = 0; i < table->count; ++i) {
    struct pollfd *user = &table->user[i];
    if (user->revents & POLLNVAL) {
      return -EBADF;
    }
    for (size_t j = 0; j < 3; ++j) {
      static const short requested[] = {POLLIN, POLLOUT, POLLPRI};
      if ((user->events & requested[j]) && (user->revents & visible[j])) {
        table->sets.result[j][user->fd / WORD_BITS] |=
            1UL << (user->fd % WORD_BITS);
        ++ready;
      }
    }
  }
  return ready;
}

static int scan(struct poll_table *table, int select) {
  int status = kernel_poll(table->kernel, table->count);
  if (status < 0) {
    return status;
  }

  int ready = 0;
  for (size_t i = 0; i < table->count; ++i) {
    table->user[i].revents |= table->kernel[i].revents;
    ready += table->user[i].revents != 0;
  }
  return select ? select_result(table) : ready;
}

static int copy_sets(const struct fd_sets *sets) {
  for (size_t j = 0; j < 3; ++j) {
    if (sets->user[j]) {
      int error = gw_signal_user_copy_out(sets->user[j], sets->result[j],
                                          sets->words * sizeof(unsigned long));
      if (error) {
        return -error;
      }
    }
  }
  return 0;
}

static int copy_poll(struct pollfd *user, const struct poll_table *table) {
  for (size_t i = 0; i < table->count; ++i) {
    void *output =
        (void *)((uintptr_t)&user[i] + offsetof(struct pollfd, revents));
    int error = gw_signal_user_copy_out(output, &table->user[i].revents,
                                        sizeof user[i].revents);
    if (error) {
      return -error;
    }
  }
  return 0;
}

static size_t probes(const struct poll_table *table, struct pollfd *out,
                     size_t capacity) {
  size_t total = 0;
  for (size_t i = 0; i < table->count; ++i) {
    if (!(table->user[i].events & (POLLIN | POLLRDNORM))) {
      continue;
    }
    size_t count = gw_guest_epoll_probes(
        table->user[i].fd, total < capacity ? out + total : NULL,
        total < capacity ? capacity - total : 0);
    if (count > SIZE_MAX - total) {
      return SIZE_MAX;
    }
    total += count;
  }
  return total;
}

/* Linux ignores nfds above the current fdtable allocation, even when the
 * caller allocated an fd_set larger than the public FD_SETSIZE.
 * https://github.com/torvalds/linux/blob/v6.12/fs/select.c
 * https://github.com/torvalds/linux/blob/v6.12/fs/proc/array.c
 */
static size_t select_descriptors(int requested) {
  int fd = syscall(SYS_openat, AT_FDCWD, "/proc/thread-self/status",
                   O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return requested;
  }

  char status[1024];
  ssize_t size = syscall(SYS_read, fd, status, sizeof status - 1);
  syscall(SYS_close, fd);
  if (size <= 0) {
    return requested;
  }
  status[size] = 0;
  char *field = strstr(status, "\nFDSize:");
  if (!field) {
    return requested;
  }

  field += sizeof "\nFDSize:" - 1;
  while (*field == '\t' || *field == ' ') {
    ++field;
  }
  unsigned long available = 0;
  while (*field >= '0' && *field <= '9' && available < (unsigned)requested) {
    available = available * 10 + (*field++ - '0');
  }
  return available && available < (unsigned)requested ? available
                                                      : (size_t)requested;
}

static int import_sets(struct poll_table *table) {
  struct fd_sets *sets = &table->sets;
  size_t length = sets->words * sizeof(unsigned long);
  for (size_t j = 0; j < 3; ++j) {
    if (sets->user[j]) {
      int error =
          gw_signal_user_copy_in(sets->result[j], sets->user[j], length);
      if (error) {
        return -error;
      }
    }
  }

  static const short events[] = {POLLIN | POLLRDNORM | POLLRDBAND,
                                 POLLOUT | POLLWRNORM | POLLWRBAND, POLLPRI};
  for (size_t word = 0; word < sets->words; ++word) {
    unsigned long bits[3];
    for (size_t j = 0; j < 3; ++j) {
      bits[j] = sets->result[j] ? sets->result[j][word] : 0;
    }
    unsigned long present = bits[0] | bits[1] | bits[2];
    while (present) {
      int number = __builtin_ctzl(present);
      unsigned long bit = 1UL << number;
      size_t fd = word * WORD_BITS + number;
      if (fd >= sets->descriptors) {
        break;
      }

      short interest = 0;
      for (size_t j = 0; j < 3; ++j) {
        if (bits[j] & bit) {
          interest |= events[j];
        }
      }
      table->user[table->count++] =
          (struct pollfd){.fd = (int)fd, .events = interest};
      present &= ~bit;
    }
  }
  return 0;
}

static int table_size(size_t count, size_t sets, size_t extra, size_t *out) {
  if (sets > SIZE_MAX / (3 * sizeof(unsigned long)) ||
      extra > SIZE_MAX / sizeof(struct pollfd) - 1 ||
      count > (SIZE_MAX / sizeof(struct pollfd) - 1 - extra) / 2) {
    return -ENOMEM;
  }
  size_t poll_bytes = (count * 2 + 1 + extra) * sizeof(struct pollfd);
  size_t set_bytes = sets * 3 * sizeof(unsigned long);
  if (set_bytes > SIZE_MAX - poll_bytes) {
    return -ENOMEM;
  }
  *out = set_bytes + poll_bytes;
  return 0;
}

static int prepare_table(struct descriptor_wait *wait, struct poll_table *table,
                         struct pollfd *user, size_t capacity, size_t extra) {
  size_t bytes;
  int status = table_size(capacity, table->sets.words, extra, &bytes);
  if (status) {
    return status;
  }
  int imported = table->user != NULL;
  unsigned long *storage = gw_guest_fd_lease_reserve(wait->lease, bytes);
  if (!storage) {
    return -errno;
  }

  for (size_t j = 0; j < 3; ++j) {
    table->sets.result[j] = table->sets.user[j] ? storage : NULL;
    storage += table->sets.words;
  }
  table->user = (struct pollfd *)storage;
  table->kernel = table->user + capacity;
  if (!imported) {
    table->count = 0;
    if (!user) {
      return import_sets(table);
    }
    table->count = capacity;
    return -gw_signal_user_copy_in(table->user, user, capacity * sizeof *user);
  }
  return 0;
}

static int wait_table(struct descriptor_wait *wait, struct pollfd *user,
                      size_t capacity, struct fd_sets sets, int *handled) {
  int select = !user;
  struct poll_table table = {.sets = sets};
  int result =
      gw_guest_fd_lease_open(&wait->lease, wait->target, -1, (uintptr_t)wait);
  if (result < 0 || (result = prepare_table(wait, &table, user, capacity, 0))) {
    return result;
  }

  for (;;) {
    struct timespec left;
    int time_left = remaining(&wait->clock, &left);
    if (time_left < 0) {
      return time_left;
    }

    gw_guest_fds_lock();
    reap();
    int managed = poll_plan(&table, wait);
    if (!managed && !*handled) {
      gw_guest_fds_unlock();
      return 0;
    }
    *handled = 1;
    /* Subscribe, resample, then retest readiness. Holding the descriptor
     * writer across registration orders ctl/mask/close with both scans. */
    if (!(result = sample(wait, time_left != 0))) {
      poll_plan(&table, wait);
      result = scan(&table, select);
    }
    if (result || !time_left || wait->interrupted) {
      gw_guest_fds_unlock();
      if (!result && wait->interrupted) {
        return -EINTR;
      }
      if (result >= 0) {
        int status = select ? copy_sets(&table.sets) : copy_poll(user, &table);
        if (status) {
          return status;
        }
      }
      return result;
    }

    size_t extra = probes(&table, NULL, 0);
    if (extra) {
      result = prepare_table(wait, &table, user, capacity, extra);
      if (result) {
        gw_guest_fds_unlock();
        return result;
      }
    }
    size_t count = 0;
    for (size_t i = 0; i < table.count; ++i) {
      if (table.kernel[i].fd >= 0) {
        table.kernel[count++] = table.kernel[i];
      }
    }
    if (extra) {
      count += probes(&table, table.kernel + count, extra);
    }
    table.kernel[count++] =
        (struct pollfd){.fd = wait->wake_fd, .events = POLLIN};
    gw_guest_fds_unlock();

    result = sleep_until_change(wait, table.kernel, count);
    if (result) {
      return result;
    }
  }
}

int gw_guest_poll(struct gw_signal_target *target, struct pollfd *fds,
                  nfds_t count, const struct timespec *timeout,
                  const sigset_t *mask, gw_libc_poll_wait poll, int *handled) {
  *handled = 0;
  if (!count || !fds) {
    return 0;
  }

  int saved = errno;
  struct rlimit limit;
  if (getrlimit(RLIMIT_NOFILE, &limit) || count > limit.rlim_cur) {
    errno = saved;
    return 0;
  }

  struct descriptor_wait wait
      __attribute__((cleanup(end))) = {.target = target, .poll = poll};
  int status = begin(&wait, timeout, mask);
  if (!status) {
    status = wait_table(&wait, fds, count, (struct fd_sets){0}, handled);
    if (*handled) {
      expose_ready_mask(&wait, mask, status);
    }
    end(&wait);
  }
  *handled |= status < 0;
  return finish(status, saved);
}

int gw_guest_select(struct gw_signal_target *target, int nfds, fd_set *read,
                    fd_set *write, fd_set *except, struct timespec *timeout,
                    const sigset_t *mask, gw_libc_poll_wait poll,
                    int *handled) {
  *handled = 0;
  if (nfds <= 0 || (!read && !write && !except)) {
    return 0;
  }

  int saved = errno;
  struct descriptor_wait wait
      __attribute__((cleanup(end))) = {.target = target, .poll = poll};
  int status = begin(&wait, timeout, mask);
  if (!status) {
    size_t number = select_descriptors(nfds);
    struct fd_sets sets = {.user = {read, write, except},
                           .descriptors = number,
                           .words = (number + WORD_BITS - 1) / WORD_BITS};
    status = wait_table(&wait, NULL, number, sets, handled);
    if (*handled && timeout) {
      int error = remaining(&wait.clock, timeout);
      if (status >= 0 && error < 0) {
        status = error;
      }
    }
    if (*handled) {
      expose_ready_mask(&wait, mask, status);
    }
    end(&wait);
  }
  *handled |= status < 0;
  return finish(status, saved);
}

static int wait_epoll(struct descriptor_wait *wait, int epfd,
                      struct epoll_event *events, int maximum) {
  for (;;) {
    struct timespec left;
    int time_left = remaining(&wait->clock, &left);
    if (time_left < 0) {
      return time_left;
    }

    gw_guest_fds_lock();
    reap();
    int result = sample(wait, time_left != 0);
    if (!result) {
      result = gw_guest_epoll_events(epfd, events, maximum, &wait->sample);
    }
    if (result || !time_left || wait->interrupted) {
      gw_guest_fds_unlock();
      return result || !wait->interrupted ? result : -EINTR;
    }

    size_t extra = gw_guest_epoll_probes(epfd, NULL, 0);
    struct pollfd *fds = NULL;
    if (extra <= SIZE_MAX / sizeof *fds - 2) {
      fds = gw_guest_fd_lease_reserve(wait->lease, (extra + 2) * sizeof *fds);
    }
    if (!fds) {
      int error = extra > SIZE_MAX / sizeof *fds - 2 ? ENOMEM : errno;
      gw_guest_fds_unlock();
      return -error;
    }
    fds[0] = (struct pollfd){.fd = epfd, .events = POLLIN};
    fds[1] = (struct pollfd){.fd = wait->wake_fd, .events = POLLIN};
    gw_guest_epoll_probes(epfd, fds + 2, extra);
    gw_guest_fds_unlock();

    result = sleep_until_change(wait, fds, extra + 2);
    if (result) {
      return result;
    }
  }
}

int gw_guest_epoll_wait(struct gw_signal_target *target, int fd,
                        struct epoll_event *events, int maximum,
                        const struct timespec *timeout, const sigset_t *mask,
                        gw_libc_poll_wait poll) {
  if (maximum <= 0 || (unsigned)maximum > INT_MAX / sizeof *events) {
    return finish(-EINVAL, 0);
  }

  if (fd < 0) {
    return finish(-EBADF, 0);
  }

  int saved = errno;
  struct descriptor_wait wait
      __attribute__((cleanup(end))) = {.target = target, .poll = poll};
  int status = begin(&wait, timeout, mask);
  if (!status) {
    int epfd =
        gw_guest_fd_lease_open(&wait.lease, target, fd, (uintptr_t)&wait);
    status = epfd < 0 ? epfd : wait_epoll(&wait, epfd, events, maximum);
    expose_ready_mask(&wait, mask, status);
    end(&wait);
  }
  return finish(status, saved);
}
