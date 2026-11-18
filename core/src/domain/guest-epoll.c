#define _GNU_SOURCE
#include "guest-epoll.h"
#include "guest-fd-state.h"
#include "guest-signalfd.h"
#include "nonlocal-mask.h"
#include "signal-user-copy.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>

struct epoll_source {
  struct epoll_file *child;
  uint64_t cookie, generation, observed, revision;
  struct epoll_event event;
  int number, probe, seen, disabled;
};

struct epoll_file {
  struct epoll_file *next;
  struct epoll_source *watches;
  off_t label;
  size_t count, capacity, cursor;
  int normal_first;
};

static int label(int fd, int command, off_t offset) {
  struct flock lock = {
      .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = offset, .l_len = 1};
  if (syscall(SYS_fcntl, fd, command, &lock) < 0) {
    return -errno;
  }
  return lock.l_type;
}

/* A Linux eventpoll open file description can own a byte-range OFD lock.
 * A probe on a different eventpoll file sees the lock; an alias of its owner
 * does not. The kernel drops it at final fput, even after SCM_RIGHTS or dup2.
 * https://github.com/torvalds/linux/blob/v6.12/fs/locks.c
 * https://github.com/torvalds/linux/blob/v5.4/fs/locks.c
 */
static int present(const struct epoll_file *file) {
  struct gw_guest_fd_state *state = gw_guest_fds_state();
  return label(state->monitor, F_OFD_GETLK, file->label) == F_WRLCK;
}

int gw_guest_epoll_valid(int fd) {
  struct gw_guest_fd_state *state = gw_guest_fds_state();
  struct stat observed;
  if (fstat(fd, &observed)) {
    return -errno;
  }
  if (observed.st_mode & S_IFMT) {
    return -EINVAL;
  }

  if (state->monitor < 0) {
    state->monitor = syscall(SYS_epoll_create1, EPOLL_CLOEXEC);
    if (state->monitor < 0) {
      return -errno;
    }
    struct stat identity;
    if (fstat(state->monitor, &identity)) {
      int error = errno;
      syscall(SYS_close, state->monitor);
      state->monitor = -1;
      return -error;
    }
    state->monitor_device = identity.st_dev;
    state->monitor_inode = identity.st_ino;
  }

  /* An unused private epoll, watched by no application, distinguishes a real
   * eventpoll from other anon_inode files sharing its st_dev/st_ino. */
  if (syscall(SYS_epoll_ctl, fd, EPOLL_CTL_DEL, state->monitor, NULL) < 0 &&
      errno == ENOENT) {
    return 1;
  }
  return -errno;
}

static struct epoll_file *find(int fd) {
  struct gw_guest_fd_state *state = gw_guest_fds_state();
  struct stat observed;
  if (!state->epolls || fstat(fd, &observed) ||
      observed.st_dev != state->monitor_device ||
      observed.st_ino != state->monitor_inode) {
    return NULL;
  }
  for (struct epoll_file *file = state->epolls; file; file = file->next) {
    if (label(fd, F_OFD_GETLK, file->label) == F_UNLCK && present(file)) {
      return file;
    }
  }
  return NULL;
}

int gw_guest_epoll_known(int fd) {
  return find(fd) != NULL;
}

static int attach(int fd, struct epoll_file **output) {
  struct gw_guest_fd_state *state = gw_guest_fds_state();
  struct epoll_file *file = find(fd);
  if (file) {
    *output = file;
    return 0;
  }

  file = mmap(NULL, sizeof *file, PROT_READ | PROT_WRITE,
              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (file == MAP_FAILED) {
    return -errno;
  }

  int status;
  do {
    if (state->epoll_label == INT64_MAX - 1) {
      status = -EOVERFLOW;
      break;
    }
    file->label = ++state->epoll_label;
    status = label(state->monitor, F_OFD_GETLK, file->label);
  } while (status == F_WRLCK || status == F_RDLCK);

  if (status >= 0) {
    status = label(fd, F_OFD_SETLK, file->label);
  }
  if (status < 0) {
    munmap(file, sizeof *file);
    return status;
  }

  file->next = state->epolls;
  state->epolls = file;
  atomic_fetch_add_explicit(&state->epoll_count, 1, memory_order_release);
  *output = file;
  return 0;
}

static void erase(struct epoll_file *file, size_t index) {
  int probe = file->watches[index].probe;
  if (probe >= 0) {
    syscall(SYS_close, probe);
  }

  --file->count;
  memmove(file->watches + index, file->watches + index + 1,
          (file->count - index) * sizeof *file->watches);
  if (file->cursor > index) {
    --file->cursor;
  }
  if (file->cursor >= file->count) {
    file->cursor = 0;
  }
}

static void destroy(struct epoll_file *file) {
  for (size_t i = 0; i < file->count; ++i) {
    if (file->watches[i].probe >= 0) {
      syscall(SYS_close, file->watches[i].probe);
    }
  }
  if (file->watches) {
    munmap(file->watches, file->capacity * sizeof *file->watches);
  }
  munmap(file, sizeof *file);
}

int gw_guest_epoll_reap(void) {
  struct gw_guest_fd_state *state = gw_guest_fds_state();
  int removed = 0;
  for (struct epoll_file **link = &state->epolls; *link;) {
    struct epoll_file *file = *link;
    if (present(file)) {
      link = &file->next;
      continue;
    }

    *link = file->next;
    atomic_fetch_sub_explicit(&state->epoll_count, 1, memory_order_release);
    removed = 1;
    for (struct epoll_file *parent = state->epolls; parent;
         parent = parent->next) {
      for (size_t i = parent->count; i; --i) {
        if (parent->watches[i - 1].child == file) {
          erase(parent, i - 1);
        }
      }
    }
    destroy(file);
  }
  return removed;
}

static void prune_signals(struct epoll_file *file) {
  for (size_t i = file->count; i; --i) {
    uint64_t mask;
    uint64_t cookie = file->watches[i - 1].cookie;
    if (cookie && !gw_guest_signalfd_mask(cookie, &mask)) {
      erase(file, i - 1);
    }
  }
}

static int reserve(struct epoll_file *file) {
  if (file->count < file->capacity) {
    return 0;
  }
  if (file->capacity > SIZE_MAX / 2 / sizeof *file->watches) {
    return -EOVERFLOW;
  }

  size_t capacity = file->capacity ? file->capacity * 2 : 8;
  struct epoll_source *array =
      mmap(NULL, capacity * sizeof *array, PROT_READ | PROT_WRITE,
           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (array == MAP_FAILED) {
    return -errno;
  }

  if (file->watches) {
    memcpy(array, file->watches, file->count * sizeof *array);
    munmap(file->watches, file->capacity * sizeof *array);
  }
  file->watches = array;
  file->capacity = capacity;
  return 0;
}

static int native_ctl(int epfd, int op, int fd, const struct epoll_event *ev) {
  return syscall(SYS_epoll_ctl, epfd, op, fd, ev) < 0 ? -errno : 0;
}

static int update_probe(int epfd, int op, struct epoll_source *watch) {
  /* Keep one native watch for the user-visible open file description and
   * one private watch for native readiness. The poll adapter merges logical
   * and native readiness into a single report, including edge and oneshot.
   * https://github.com/torvalds/linux/blob/v6.12/fs/eventpoll.c
   */
  struct epoll_event quiet = {0};
  if (watch->cookie) {
    quiet.events = watch->event.events &
                   (EPOLLET | EPOLLONESHOT | EPOLLEXCLUSIVE | EPOLLWAKEUP);
  }
  int result = native_ctl(epfd, op, watch->number, &quiet);
  if (result || op == EPOLL_CTL_DEL) {
    return result;
  }

  int backing =
      watch->cookie ? gw_guest_signalfd_native(watch->cookie) : watch->number;
  int interested =
      watch->event.events & (watch->child ? EPOLLIN | EPOLLRDNORM : EPOLLIN);
  struct epoll_event event = {
      .events = interested ? EPOLLIN | (watch->event.events & EPOLLET) : 0};
  if (op == EPOLL_CTL_ADD) {
    watch->probe = syscall(SYS_epoll_create1, EPOLL_CLOEXEC);
  }
  if (watch->probe < 0) {
    result = -errno;
  } else {
    result = native_ctl(watch->probe, op, backing, &event);
  }
  if (result && op == EPOLL_CTL_ADD) {
    if (watch->probe >= 0) {
      syscall(SYS_close, watch->probe);
      watch->probe = -1;
    }
    native_ctl(epfd, EPOLL_CTL_DEL, watch->number, NULL);
  }
  return result;
}

static int change_watch(int epfd, struct epoll_file *file, int op,
                        struct epoll_source source,
                        const struct epoll_event *user) {
  struct gw_guest_fd_state *state = gw_guest_fds_state();
  prune_signals(file);
  size_t index = 0;
  while (index < file->count && (file->watches[index].number != source.number ||
                                 file->watches[index].cookie != source.cookie ||
                                 file->watches[index].child != source.child)) {
    ++index;
  }

  int exists = index < file->count;
  if (op != EPOLL_CTL_DEL && op != EPOLL_CTL_MOD && op != EPOLL_CTL_ADD) {
    return -EINVAL;
  }
  if (op != EPOLL_CTL_DEL) {
    int error =
        gw_signal_user_copy_in(&source.event, user, sizeof source.event);
    if (error) {
      return -error;
    }
    const uint32_t exclusive_bits = EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP |
                                    EPOLLWAKEUP | EPOLLET | EPOLLEXCLUSIVE;
    if ((source.event.events & EPOLLEXCLUSIVE) &&
        (op != EPOLL_CTL_ADD || source.child ||
         (source.event.events & ~exclusive_bits))) {
      return -EINVAL;
    }
  }
  if (op == EPOLL_CTL_ADD ? exists : !exists) {
    return exists ? -EEXIST : -ENOENT;
  }
  if (exists && op == EPOLL_CTL_MOD &&
      (file->watches[index].event.events & EPOLLEXCLUSIVE)) {
    return -EINVAL;
  }

  int status = 0;
  if (op == EPOLL_CTL_ADD) {
    status = reserve(file);
  } else {
    source.probe = file->watches[index].probe;
  }
  if (!status) {
    status = update_probe(epfd, op, &source);
  }
  if (status) {
    return status;
  }
  if (op == EPOLL_CTL_DEL) {
    erase(file, index);
  } else {
    source.revision = ++state->epoll_revision;
    file->watches[index] = source;
    file->count += !exists;
  }
  return 0;
}

static int control(int epfd, int op, int fd, const struct epoll_event *event) {
  uint64_t cookie = gw_guest_signalfd_cookie(fd);
  if (!cookie && gw_guest_epoll_valid(fd) != 1) {
    return native_ctl(epfd, op, fd, event);
  }

  int status = gw_guest_epoll_valid(epfd);
  if (status < 0) {
    return status;
  }
  if (epfd == fd) {
    return -EINVAL;
  }

  struct epoll_file *file, *child = NULL;
  status = attach(epfd, &file);
  if (!status && !cookie) {
    status = attach(fd, &child);
  }
  if (status) {
    return status;
  }
  if (file == child) {
    return -EINVAL;
  }

  struct epoll_source source = {
      .child = child, .cookie = cookie, .number = fd, .probe = -1};
  return change_watch(epfd, file, op, source, event);
}

int gw_guest_epoll_ctl(struct gw_signal_targets *targets, int epfd, int op,
                       int fd, const struct epoll_event *event) {
  uint64_t physical = gw_nonlocal_mask_block();
  gw_guest_fds_lock();
  if (gw_guest_fds_state()->closed) {
    gw_guest_fds_unlock();
    gw_nonlocal_mask_restore(physical);
    return native_ctl(epfd, op, fd, event);
  }
  gw_guest_signalfd_reap();
  gw_guest_epoll_reap();
  int result = control(epfd, op, fd, event);
  if (!result) {
    gw_signal_targets_wake_waiters(targets);
  }
  gw_guest_fds_unlock();
  gw_nonlocal_mask_restore(physical);
  return result;
}

static int probe_ready(int fd) {
  struct pollfd probe = {.fd = fd, .events = POLLIN};
  struct timespec zero = {0};
  return syscall(SYS_ppoll, &probe, 1, &zero, NULL, 8) == 1 &&
         (probe.revents & POLLIN);
}

static uint64_t ready_revision(struct epoll_file *,
                               const struct gw_signal_poll_sample *);

static uint32_t readable_events(const struct epoll_source *watch) {
  return watch->event.events & (watch->child ? EPOLLIN | EPOLLRDNORM : EPOLLIN);
}

static int watch_ready(struct epoll_source *watch,
                       const struct gw_signal_poll_sample *sample,
                       uint64_t *child_revision) {
  if (watch->disabled || !readable_events(watch)) {
    return 0;
  }

  uint64_t mask;
  int logical;
  if (watch->child) {
    *child_revision = ready_revision(watch->child, sample);
    logical = *child_revision != 0;
  } else {
    logical = gw_guest_signalfd_mask(watch->cookie, &mask) &&
              (sample->pending & mask);
  }
  if (probe_ready(watch->probe)) {
    return 1;
  }

  /* Linux signalfd uses a shared waitqueue. Each actual enqueue or mask
   * update can requeue an edge even while an older matching signal remains.
   * A nested epoll can also become ready via ADD/MOD without a new signal.
   * https://github.com/torvalds/linux/blob/v6.12/fs/signalfd.c
   * https://github.com/torvalds/linux/blob/v6.12/fs/eventpoll.c
   */
  return logical && (!(watch->event.events & EPOLLET) || !watch->seen ||
                     watch->generation != sample->generation ||
                     *child_revision > watch->observed);
}

static uint64_t ready_revision(struct epoll_file *file,
                               const struct gw_signal_poll_sample *sample) {
  uint64_t latest = 0;
  for (size_t i = 0; i < file->count; ++i) {
    struct epoll_source *watch = &file->watches[i];
    uint64_t child = 0;
    if (watch_ready(watch, sample, &child)) {
      uint64_t ready = watch->revision > child ? watch->revision : child;
      if (ready > latest) {
        latest = ready;
      }
    }
  }
  return latest;
}

int gw_guest_epoll_ready(int fd, const struct gw_signal_poll_sample *sample) {
  struct epoll_file *file = find(fd);
  return file && ready_revision(file, sample);
}

static int native_events(int epfd, struct epoll_event *events, int maxevents) {
  int result = syscall(SYS_epoll_pwait, epfd, events, maxevents, 0, NULL, 8);
  return result < 0 ? -errno : result;
}

static int logical_events(struct epoll_file *file, struct epoll_event *user,
                          int maximum,
                          const struct gw_signal_poll_sample *sample) {
  if (!maximum) {
    return 0;
  }
  int written = 0;
  size_t index = file->cursor;
  for (size_t visited = 0; visited < file->count; ++visited) {
    struct epoll_source *watch = &file->watches[index];
    index = (index + 1) % file->count;
    uint64_t child_revision = 0;
    if (!watch_ready(watch, sample, &child_revision)) {
      continue;
    }

    struct epoll_event event = watch->event;
    event.events = readable_events(watch);
    int error = gw_signal_user_copy_out(user + written, &event, sizeof event);
    if (error) {
      return written ? written : -error;
    }
    struct epoll_event ignored;
    native_events(watch->probe, &ignored, 1);
    watch->generation = sample->generation;
    watch->observed = child_revision;
    watch->seen = 1;
    watch->disabled = (watch->event.events & EPOLLONESHOT) != 0;
    file->cursor = index;
    if (++written == maximum) {
      break;
    }
  }
  return written;
}

int gw_guest_epoll_events(int fd, struct epoll_event *events, int maximum,
                          const struct gw_signal_poll_sample *sample) {
  struct epoll_file *file = find(fd);
  if (!file || !file->count) {
    return native_events(fd, events, maximum);
  }

  int first = 0, second = 0;
  if (file->normal_first) {
    first = native_events(fd, events, maximum);
    if (first >= 0 && first < maximum) {
      second = logical_events(file, events + first, maximum - first, sample);
    }
  } else {
    first = logical_events(file, events, maximum, sample);
    if (first >= 0 && first < maximum) {
      second = native_events(fd, events + first, maximum - first);
    }
  }
  file->normal_first = !file->normal_first;
  return first <= 0 ? (first ? first : second)
                    : first + (second > 0 ? second : 0);
}

static size_t collect_probes(struct epoll_file *file, struct pollfd *out,
                             size_t cap) {
  size_t count = 0;
  for (size_t i = 0; i < file->count; ++i) {
    struct epoll_source *watch = &file->watches[i];
    if (watch->disabled || !readable_events(watch)) {
      continue;
    }
    if (count < cap) {
      out[count] = (struct pollfd){.fd = watch->probe, .events = POLLIN};
    }
    ++count;
    if (!watch->child) {
      continue;
    }
    size_t added =
        collect_probes(watch->child, count < cap ? out + count : NULL,
                       count < cap ? cap - count : 0);
    if (added > SIZE_MAX - count) {
      return SIZE_MAX;
    }
    count += added;
  }
  return count;
}

size_t gw_guest_epoll_probes(int fd, struct pollfd *out, size_t capacity) {
  struct epoll_file *file = find(fd);
  return file ? collect_probes(file, out, capacity) : 0;
}

int gw_guest_epoll_stop(void) {
  struct gw_guest_fd_state *state = gw_guest_fds_state();
  gw_guest_epoll_reap();
  for (struct epoll_file *file = state->epolls; file; file = file->next) {
    prune_signals(file);
    if (file->count) {
      return EBUSY;
    }
  }
  while (state->epolls) {
    struct epoll_file *file = state->epolls;
    state->epolls = file->next;
    destroy(file);
  }
  atomic_store_explicit(&state->epoll_count, 0, memory_order_release);
  if (state->monitor >= 0) {
    syscall(SYS_close, state->monitor);
    state->monitor = -1;
  }
  return 0;
}
