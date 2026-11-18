#define _GNU_SOURCE
#include "guest-signalfd.h"
#include "guest-fd-lease.h"
#include "nonlocal-mask.h"
#include "signal-user-copy.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sys/socket.h>

_Static_assert(sizeof(struct signalfd_siginfo) == 128, "Linux signalfd record");

/* The application endpoint's cookie follows its open file description. The
 * private peer observes the last kernel reference, including imported or
 * duplicated descriptors. A number reused by close/dup2 cannot match a cookie.
 * https://github.com/torvalds/linux/blob/v6.12/net/core/sock.c
 * https://github.com/torvalds/linux/blob/v6.12/net/unix/af_unix.c
 */
struct signal_fd {
  uint64_t cookie, peer_cookie, mask;
  int peer;
};

static struct signal_fd *descriptors;
static size_t capacity;
static _Atomic size_t count;
static atomic_flag writer = ATOMIC_FLAG_INIT;

enum { VECTORS_SCATTER = 1, VECTORS_SEPARATE };

static _Atomic int layout;

static int probe_vectors(void) {
  uint64_t empty = 0;
  int fd = syscall(SYS_signalfd4, -1, &empty, sizeof empty,
                   SFD_CLOEXEC | SFD_NONBLOCK);
  if (fd < 0) {
    return -errno;
  }

  /* Before read_iter, the VFS passed one vector at a time to signalfd. Test
   * the actual kernel because an Android kernel can backport either path.
   * https://github.com/torvalds/linux/blob/v5.4/fs/read_write.c
   */
  char half[sizeof(struct signalfd_siginfo) / 2];
  const struct iovec split[] = {{half, sizeof half}, {half, sizeof half}};
  long result = syscall(SYS_readv, fd, split, 2);
  int error = result < 0 ? errno : EIO;
  syscall(SYS_close, fd);
  return error == EAGAIN   ? VECTORS_SCATTER
         : error == EINVAL ? VECTORS_SEPARATE
                           : -error;
}

static uint64_t socket_cookie(int descriptor) {
  uint64_t cookie;
  socklen_t size = sizeof cookie;
  return syscall(SYS_getsockopt, descriptor, SOL_SOCKET, SO_COOKIE, &cookie,
                 &size) ||
                 size != sizeof cookie
             ? 0
             : cookie;
}

void gw_guest_fds_lock(void) {
  while (atomic_flag_test_and_set_explicit(&writer, memory_order_acquire)) {
  }
}

void gw_guest_fds_unlock(void) {
  atomic_flag_clear_explicit(&writer, memory_order_release);
}

static int vector_layout(void) {
  int result = atomic_load_explicit(&layout, memory_order_acquire);
  if (result) {
    return result;
  }

  uint64_t physical = gw_nonlocal_mask_block();
  gw_guest_fds_lock();
  result = atomic_load_explicit(&layout, memory_order_relaxed);
  if (!result) {
    result = probe_vectors();
    if (result > 0) {
      atomic_store_explicit(&layout, result, memory_order_release);
    }
  }
  gw_guest_fds_unlock();
  gw_nonlocal_mask_restore(physical);
  return result;
}

static size_t position(uint64_t cookie) {
  size_t low = 0, high = count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    if (descriptors[middle].cookie < cookie) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  return low;
}

static struct signal_fd *find(uint64_t cookie) {
  size_t index = position(cookie);
  return index < count && descriptors[index].cookie == cookie
             ? &descriptors[index]
             : NULL;
}

static int retired(const struct signal_fd *fd) {
  if (socket_cookie(fd->peer) != fd->peer_cookie) {
    return 0;
  }
  struct pollfd peer = {.fd = fd->peer};
  struct timespec immediate = {0};
  if (syscall(SYS_ppoll, &peer, 1, &immediate, NULL, 8) == 1 &&
      (peer.revents & POLLHUP)) {
    syscall(SYS_close, fd->peer);
    return 1;
  }
  return 0;
}

int gw_guest_signalfd_reap(void) {
  size_t retained = 0;
  for (size_t i = 0; i < count; ++i) {
    if (!retired(&descriptors[i])) {
      descriptors[retained++] = descriptors[i];
    }
  }
  return atomic_exchange_explicit(&count, retained, memory_order_release) !=
         retained;
}

uint64_t gw_guest_signalfd_cookie(int descriptor) {
  uint64_t cookie = count ? socket_cookie(descriptor) : 0;
  return cookie && find(cookie) ? cookie : 0;
}

int gw_guest_signalfd_mask(uint64_t cookie, uint64_t *mask) {
  struct signal_fd *fd = find(cookie);
  if (!fd) {
    return 0;
  }
  *mask = fd->mask;
  return 1;
}

static int reserve(void) {
  if (count < capacity) {
    return 0;
  }
  if (capacity > SIZE_MAX / 2 / sizeof *descriptors) {
    return EOVERFLOW;
  }
  size_t next = capacity ? 2 * capacity : 16;
  struct signal_fd *entries =
      mmap(NULL, next * sizeof *entries, PROT_READ | PROT_WRITE,
           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (entries == MAP_FAILED) {
    return errno;
  }
  if (descriptors) {
    memcpy(entries, descriptors, count * sizeof *entries);
    munmap(descriptors, capacity * sizeof *entries);
  }
  descriptors = entries;
  capacity = next;
  return 0;
}

static int create(uint64_t mask, int flags) {
  gw_guest_signalfd_reap();
  int error = reserve();
  if (error) {
    return -error;
  }

  int pair[2];
  int type = SOCK_STREAM | SOCK_CLOEXEC;
  if (flags & SFD_NONBLOCK) {
    type |= SOCK_NONBLOCK;
  }
  if (syscall(SYS_socketpair, AF_UNIX, type, 0, pair)) {
    return -errno;
  }

  struct signal_fd entry = {.cookie = socket_cookie(pair[0]),
                            .peer_cookie = socket_cookie(pair[1]),
                            .mask = mask,
                            .peer = pair[1]};
  if (!entry.cookie || !entry.peer_cookie) {
    error = ENOTSUP;
  } else if (!(flags & SFD_CLOEXEC) &&
             syscall(SYS_fcntl, pair[0], F_SETFD, 0) < 0) {
    error = errno;
  }
  if (error) {
    syscall(SYS_close, pair[0]);
    syscall(SYS_close, pair[1]);
    return -error;
  }

  size_t index = position(entry.cookie);
  memmove(descriptors + index + 1, descriptors + index,
          (count - index) * sizeof *descriptors);
  descriptors[index] = entry;
  atomic_fetch_add_explicit(&count, 1, memory_order_release);
  return pair[0];
}

int gw_guest_signalfd(struct gw_signal_target *target, int descriptor,
                      const sigset_t *set, int flags, int *handled) {
  int saved = errno;
  uint64_t physical = gw_nonlocal_mask_block();
  gw_guest_fds_lock();
  struct signal_fd *existing =
      descriptor >= 0 && count ? find(socket_cookie(descriptor)) : NULL;
  *handled = descriptor == -1 || existing != NULL;
  int result = 0;
  if (*handled) {
    uint64_t mask;
    int error = flags & ~(SFD_NONBLOCK | SFD_CLOEXEC)
                    ? EINVAL
                    : gw_signal_user_copy_in(&mask, set, sizeof mask);
    if (error) {
      result = -error;
    } else {
      mask &= ~target->registry->inbox.reserved;
      mask &=
          ~((UINT64_C(1) << (SIGKILL - 1)) | (UINT64_C(1) << (SIGSTOP - 1)));
      if (existing) {
        /* Linux updates the mask for the description and wakes current reads.
         * The update ignores the close-on-exec/nonblocking creation flags.
         * https://github.com/torvalds/linux/blob/v6.12/fs/signalfd.c */
        existing->mask = mask;
        gw_signal_targets_poll_changed(target->registry);
        result = descriptor;
      } else {
        result = create(mask, flags);
      }
    }
  }
  gw_guest_fds_unlock();
  errno = result < 0 ? -result : saved;
  gw_nonlocal_mask_restore(physical);
  return result < 0 ? -1 : result;
}

static void encode(struct signalfd_siginfo *record, const siginfo_t *signal) {
  /* Tagged libc sends carry the Linux KILL/RT layouts. Preserve SI_TKILL
   * here, including when sigtimedwait would normalize it to SI_USER.
   * https://github.com/torvalds/linux/blob/v6.12/fs/signalfd.c */
  memset(record, 0, sizeof *record);
  record->ssi_signo = signal->si_signo;
  record->ssi_errno = signal->si_errno;
  record->ssi_code = signal->si_code;
  record->ssi_pid = signal->si_pid;
  record->ssi_uid = signal->si_uid;
  if (signal->si_code == SI_QUEUE) {
    record->ssi_int = signal->si_value.sival_int;
    record->ssi_ptr = (uintptr_t)signal->si_value.sival_ptr;
  }
}

struct signal_read {
  struct gw_guest_fd_lease *lease;
  uint64_t cookie, interrupts;
  int started, nonblocking;
};

static void release_read(struct signal_read *read) {
  gw_guest_fd_lease_drop(&read->lease);
}

struct read_output {
  const struct iovec *vector;
  size_t offset, remaining;
};

static int copy_record(struct read_output *out,
                       const struct signalfd_siginfo *record) {
  const char *bytes = (const void *)record;
  size_t remaining = sizeof *record;
  while (remaining) {
    size_t length = out->vector->iov_len - out->offset;
    if (!length) {
      ++out->vector;
      out->offset = 0;
      continue;
    }
    if (length > remaining) {
      length = remaining;
    }
    void *destination =
        (void *)((uintptr_t)out->vector->iov_base + out->offset);
    int error = gw_signal_user_copy_out(destination, bytes, length);
    if (error) {
      return error;
    }
    out->offset += length;
    remaining -= length;
    bytes += length;
  }
  out->remaining -= sizeof *record;
  return 0;
}

static int identify(struct gw_signal_target *target, struct signal_read *read,
                    int descriptor) {
  read->cookie = socket_cookie(descriptor);
  if (!read->cookie) {
    return 0;
  }
  uint64_t physical = gw_nonlocal_mask_block();
  gw_guest_fds_lock();
  struct signal_fd *fd = find(read->cookie);
  int result = 0;
  if (fd) {
    int borrowed = gw_guest_fd_lease_open(&read->lease, target, descriptor,
                                          (uintptr_t)read);
    if (borrowed < 0) {
      result = borrowed;
    } else if (socket_cookie(borrowed) != read->cookie) {
      result = -EBADF;
    } else {
      long flags = syscall(SYS_fcntl, borrowed, F_GETFL, 0);
      read->nonblocking = (flags & O_NONBLOCK) != 0;
      result = 1;
    }
  }
  gw_guest_fds_unlock();
  gw_nonlocal_mask_restore(physical);
  return result;
}

static ssize_t take(struct gw_signal_target *target, struct signal_read *read,
                    struct read_output *out, uint32_t *sequence) {
  uint64_t physical = gw_nonlocal_mask_block();
  ssize_t written = 0;
  int result = 0;
  while (out->remaining >= sizeof(struct signalfd_siginfo)) {
    siginfo_t packet;
    struct gw_signal_wait_sample sample;
    gw_guest_fds_lock();
    struct signal_fd *fd = find(read->cookie);
    result = fd ? gw_signal_target_wait_take(target, fd->mask, &packet, &sample)
                : -EBADF;
    gw_guest_fds_unlock();

    if (result <= 0) {
      if (!result) {
        *sequence = sample.sequence;
        if (!read->started) {
          read->started = 1;
          read->interrupts = sample.interrupted_io;
        } else if (read->interrupts != sample.interrupted_io) {
          result = -EINTR;
        }
      }
      break;
    }
    struct signalfd_siginfo record;
    encode(&record, &packet);
    int error = copy_record(out, &record);
    if (error) {
      result = -error;
      break;
    }
    written += sizeof record;
  }
  gw_nonlocal_mask_restore(physical);
  return written ? written : result;
}

static ssize_t read_records(struct gw_signal_target *target,
                            struct signal_read *read, struct read_output *out,
                            gw_libc_wait_event wait) {
  if (out->remaining < sizeof(struct signalfd_siginfo)) {
    return -EINVAL;
  }

  read->started = 0;
  ssize_t result;
  do {
    uint32_t sequence = 0;
    result = take(target, read, out, &sequence);
    if (result) {
      return result;
    }
    if (read->nonblocking) {
      return -EAGAIN;
    }
    result = wait(&target->registry->wait_sequence, sequence, NULL);
  } while (!result || result == -EAGAIN || result == -EINTR);
  return result;
}

static int begin_read(struct gw_signal_target *target, struct signal_read *read,
                      int descriptor, int *handled) {
  int match = atomic_load_explicit(&count, memory_order_acquire)
                  ? identify(target, read, descriptor)
                  : 0;
  *handled = match != 0;
  if (match > 0) {
    pthread_testcancel();
  }
  return match;
}

static size_t max_read(void) {
  return INT_MAX & ~((size_t)getpagesize() - 1);
}

ssize_t gw_guest_signalfd_read(struct gw_signal_target *target, int descriptor,
                               void *output, size_t length,
                               gw_libc_wait_event wait, int *handled) {
  int saved = errno;
  struct signal_read read __attribute__((cleanup(release_read))) = {0};
  ssize_t result = begin_read(target, &read, descriptor, handled);
  if (result > 0) {
    int error = gw_signal_user_validate_read(output, length);
    size_t maximum = max_read();
    struct iovec vector = {output, length > maximum ? maximum : length};
    struct read_output out = {.vector = &vector, .remaining = vector.iov_len};
    result = error ? -error : read_records(target, &read, &out, wait);
  }
  errno = result < 0 ? -result : saved;
  return result < 0 ? -1 : result;
}

static ssize_t read_separate_vectors(struct gw_signal_target *target,
                                     struct signal_read *read,
                                     const struct iovec *vectors, int count,
                                     gw_libc_wait_event wait) {
  ssize_t combined = 0;
  for (int i = 0; i < count; ++i) {
    if (i && !vectors[i].iov_len) {
      continue;
    }
    struct read_output out = {.vector = &vectors[i],
                              .remaining = vectors[i].iov_len};
    ssize_t result = read_records(target, read, &out, wait);
    if (result < 0) {
      return combined ? combined : result;
    }
    combined += result;
    if ((size_t)result != vectors[i].iov_len) {
      break;
    }
  }
  return combined;
}

static ssize_t read_vector(struct gw_signal_target *target,
                           struct signal_read *read, const struct iovec *user,
                           int count, gw_libc_wait_event wait) {
  int error = gw_signal_user_validate_readv(user, count);
  if (error || !count) {
    return error ? -error : 0;
  }
  /* Linux caps iovcnt at IOV_MAX. Keep the imported vectors on the owning
   * call stack: output can overlap the user array, and abandoned reads must
   * leave no allocation or descriptor for a skipped stack frame to release.
   * https://github.com/torvalds/linux/blob/v6.12/lib/iov_iter.c */
  if (count < 0 || count > IOV_MAX) {
    return -EINVAL;
  }
  struct iovec vectors[count];
  size_t left = max_read(), total = 0;
  for (int i = 0; i < count; ++i) {
    error = gw_signal_user_copy_in(&vectors[i], user + i, sizeof *vectors);
    if (error) {
      return -error;
    }
    if (vectors[i].iov_len > left) {
      vectors[i].iov_len = left;
    }
    total += vectors[i].iov_len;
    left -= vectors[i].iov_len;
  }
  if (!total) {
    return 0;
  }
  int mode = vector_layout();
  if (mode < 0) {
    return mode;
  }
  if (mode == VECTORS_SEPARATE) {
    return read_separate_vectors(target, read, vectors, count, wait);
  }

  struct read_output out = {.vector = vectors, .remaining = total};
  return read_records(target, read, &out, wait);
}

ssize_t gw_guest_signalfd_readv(struct gw_signal_target *target, int descriptor,
                                const struct iovec *vectors, int count,
                                gw_libc_wait_event wait, int *handled) {
  int saved = errno;
  struct signal_read read __attribute__((cleanup(release_read))) = {0};
  ssize_t result = begin_read(target, &read, descriptor, handled);
  if (result > 0) {
    result = read_vector(target, &read, vectors, count, wait);
  }
  errno = result < 0 ? -result : saved;
  return result < 0 ? -1 : result;
}

int gw_guest_signalfd_stop(void) {
  gw_guest_fds_lock();
  gw_guest_signalfd_reap();
  int error = count ? EBUSY : 0;
  if (!error && descriptors) {
    munmap(descriptors, capacity * sizeof *descriptors);
    descriptors = NULL;
    capacity = 0;
  }
  gw_guest_fds_unlock();
  return error;
}
