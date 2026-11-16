#define _GNU_SOURCE
#include "signal-fd-kernel.h"
#include "nonlocal-mask.h"
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

/* proc exposes the OFD's actual signalfd mask, including after SCM_RIGHTS,
 * dup, close_range, unshare and thread exit. Inspect every live task's table
 * only at serialized lifecycle boundaries. Reads inspect their own descriptor.
 * https://github.com/torvalds/linux/blob/v6.12/fs/signalfd.c
 * https://github.com/torvalds/linux/blob/v6.12/fs/proc/fd.c
 * https://github.com/torvalds/linux/blob/v6.12/fs/file.c
 */
static size_t decimal(char *buffer, unsigned number) {
  char digits[10];
  size_t size = 0;
  do {
    digits[size++] = '0' + number % 10;
    number /= 10;
  } while (number);
  for (size_t i = 0; i < size; ++i) {
    buffer[i] = digits[size - i - 1];
  }
  buffer[size] = 0;
  return size;
}

static int numeric(const char *name) {
  if (!*name) {
    return 0;
  }
  for (; *name; ++name) {
    if (*name < '0' || *name > '9') {
      return 0;
    }
  }
  return 1;
}

static int parse_mask(const char *text, size_t size, uint64_t *mask) {
  static const char key[] = "sigmask:";
  for (size_t offset = 0; offset < size;) {
    size_t end = offset;
    while (end < size && text[end] != '\n') {
      ++end;
    }
    if (end - offset > sizeof key - 1 &&
        !memcmp(text + offset, key, sizeof key - 1)) {
      size_t i = offset + sizeof key - 1;
      while (i < end && (text[i] == ' ' || text[i] == '\t')) {
        ++i;
      }
      unsigned digits = 0;
      uint64_t bits = 0;
      for (; i < end; ++i) {
        unsigned char c = text[i];
        unsigned value = c >= '0' && c <= '9'   ? c - '0'
                         : c >= 'a' && c <= 'f' ? c - 'a' + 10
                         : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                : 16;
        if (value == 16 || ++digits > 16) {
          return -1;
        }
        bits = (bits << 4) | value;
      }
      if (!digits) {
        return -1;
      }
      *mask = bits;
      return 0;
    }
    offset = end + 1;
  }
  return -1;
}

static int read_mask(int info_directory, const char *name, uint64_t *mask) {
  int info = syscall(SYS_openat, info_directory, name, O_RDONLY | O_CLOEXEC, 0);
  if (info < 0) {
    if (errno == ENOENT || errno == EBADF) {
      return 0;
    }
    errno = ENOTSUP;
    return -1;
  }
  char text[1024];
  size_t used = 0;
  int error = 0;
  while (used < sizeof text) {
    ssize_t count = syscall(SYS_read, info, text + used, sizeof text - used);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      error = ENOTSUP;
      break;
    }
    if (!count) {
      break;
    }
    used += count;
  }
  syscall(SYS_close, info);
  if (error || parse_mask(text, used, mask)) {
    errno = error ? error : ENOTSUP;
    return -1;
  }
  return 1;
}

static int inspect(int directory, const char *name) {
  char target[64];
  ssize_t size =
      syscall(SYS_readlinkat, directory, name, target, sizeof target);
  if (size < 0) {
    if (errno == ENOENT || errno == EBADF) {
      return 0;
    }
    errno = ENOTSUP;
    return -1;
  }
  static const char expected[] = "anon_inode:[signalfd]";
  return size == sizeof expected - 1 && !memcmp(target, expected, size);
}

int gw_signal_fd_kernel(int descriptor, uint64_t *mask) {
  if (descriptor < 0) {
    return 0;
  }
  int saved_errno = errno;
  uint64_t previous = gw_nonlocal_mask_block();
  char path[64];
  /* /proc/self/fd resolves the group leader's table. thread-self is essential
   * after close_range(UNSHARE), even when this is an ordinary pthread.
   * https://github.com/torvalds/linux/blob/v6.12/fs/proc/thread_self.c
   */
  static const char prefix[] = "/proc/thread-self/fd/";
  memcpy(path, prefix, sizeof prefix - 1);
  decimal(path + sizeof prefix - 1, descriptor);
  int result = inspect(AT_FDCWD, path);
  if (result == 1 && mask) {
    static const char info_prefix[] = "/proc/thread-self/fdinfo/";
    memcpy(path, info_prefix, sizeof info_prefix - 1);
    decimal(path + sizeof info_prefix - 1, descriptor);
    result = read_mask(AT_FDCWD, path, mask);
  }
  int error = result < 0 ? errno : saved_errno;
  gw_nonlocal_mask_restore(previous);
  errno = error;
  return result;
}

struct directory_entry {
  uint64_t inode;
  int64_t offset;
  unsigned short length;
  unsigned char type;
  char name[];
};

typedef int (*visit_entry)(int, const char *, uint64_t);

static int walk(int directory, uint64_t filter, visit_entry visit) {
  _Alignas(uint64_t) char bytes[8192];
  for (;;) {
    ssize_t size = syscall(SYS_getdents64, directory, bytes, sizeof bytes);
    if (size < 0) {
      if (errno == EINTR) {
        continue;
      }
      return ENOTSUP;
    }
    if (!size) {
      return 0;
    }
    for (size_t offset = 0; offset < (size_t)size;) {
      struct directory_entry *entry = (void *)(bytes + offset);
      size_t base = offsetof(struct directory_entry, name);
      if ((size_t)size - offset < base + 1 || entry->length < base + 1 ||
          entry->length > (size_t)size - offset ||
          !memchr(entry->name, 0, entry->length - base)) {
        return EIO;
      }
      if (numeric(entry->name)) {
        int result = visit(directory, entry->name, filter);
        if (result) {
          return result;
        }
      }
      offset += entry->length;
    }
  }
}

static int visit_fd(int directory, const char *name, uint64_t filter) {
  uint64_t mask = 0;
  int result = inspect(directory, name);
  if (result <= 0 || !filter) {
    return result < 0 ? ENOTSUP : result ? EBUSY : 0;
  }
  /* fdinfo is a sibling of fd under this exact task's directory. Opening
   * through the task, rather than /proc/self/fdinfo, preserves private tables.
   */
  int infos = syscall(SYS_openat, directory, "../fdinfo",
                      O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
  if (infos < 0) {
    return ENOTSUP;
  }
  result = read_mask(infos, name, &mask);
  syscall(SYS_close, infos);
  return result < 0                               ? ENOTSUP
         : result && (!filter || (mask & filter)) ? EBUSY
                                                  : 0;
}

static int visit_task(int directory, const char *name, uint64_t filter) {
  int task = syscall(SYS_openat, directory, name,
                     O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
  if (task < 0) {
    return errno == ENOENT || errno == ESRCH ? 0 : ENOTSUP;
  }
  int fds =
      syscall(SYS_openat, task, "fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
  int result = fds < 0 ? (errno == ENOENT || errno == ESRCH ? 0 : ENOTSUP)
                       : walk(fds, filter, visit_fd);
  if (fds >= 0) {
    syscall(SYS_close, fds);
  }
  syscall(SYS_close, task);
  return result;
}

int gw_signal_fd_inventory(uint64_t filter) {
  int saved_errno = errno;
  uint64_t previous = gw_nonlocal_mask_block();
  int directory = syscall(SYS_openat, AT_FDCWD, "/proc/self/task",
                          O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
  int result = directory < 0 ? ENOTSUP : walk(directory, filter, visit_task);
  if (directory >= 0) {
    syscall(SYS_close, directory);
  }
  gw_nonlocal_mask_restore(previous);
  errno = saved_errno;
  return result;
}
