#define _GNU_SOURCE
#include "signal-user-copy.h"
#include "nonlocal-mask.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/memfd.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/stat.h>
#include <time.h>

/* Kernel-mediated copies preserve wait API pointer error behavior.
 * Pre-validating a remainder on success changes nanosleep semantics;
 * a C store on interruption changes EFAULT into SIGSEGV. One preallocated OFD
 * avoids per-signal allocation and does not depend on process_vm_* permissions.
 * https://github.com/torvalds/linux/blob/v6.12/fs/read_write.c
 * https://github.com/torvalds/linux/blob/v6.12/mm/filemap.c
 * See vfs_read(), vfs_write() and filemap_read(). */
static int descriptor = -1;
static struct stat identity;
static atomic_flag writer = ATOMIC_FLAG_INIT;

static uint64_t lock(void) {
  uint64_t mask = gw_nonlocal_mask_block();
  while (atomic_flag_test_and_set_explicit(&writer, memory_order_acquire)) {
  }
  return mask;
}

static void unlock(uint64_t mask) {
  atomic_flag_clear_explicit(&writer, memory_order_release);
  gw_nonlocal_mask_restore(mask);
}

static int available(void) {
  struct stat observed;
  return descriptor >= 0 && !fstat(descriptor, &observed) &&
         observed.st_dev == identity.st_dev &&
         observed.st_ino == identity.st_ino;
}

int gw_signal_user_copy_prepare(void) {
  if (descriptor >= 0) {
    return 0; /* Serialized bootstrap; the OFD remains process-pinned. */
  }

  int fd = syscall(SYS_memfd_create, "glibc-worker-signal-copy", MFD_CLOEXEC);
  if (fd < 0) {
    return errno;
  }
  unsigned char zero[sizeof(siginfo_t)] = {0};
  int error = 0;
  if (syscall(SYS_pwrite64, fd, zero, sizeof zero, 0) != (long)sizeof zero ||
      fstat(fd, &identity)) {
    error = errno ? errno : EIO;
  }
  if (error) {
    syscall(SYS_close, fd);
    return error;
  }

  descriptor = fd;
  return 0;
}

static int copy(void *destination, const void *source, size_t length, int out) {
  if (length > sizeof(siginfo_t)) {
    return EINVAL;
  }

  int saved = errno;
  uint64_t mask = lock();
  int error = available() ? 0 : ENOTSUP;
  if (!error &&
      syscall(SYS_pwrite64, descriptor, source, length, 0) != (long)length) {
    error = out ? EIO : EFAULT;
  }
  if (!error && syscall(SYS_pread64, descriptor, destination, length, 0) !=
                    (long)length) {
    error = out ? EFAULT : EIO;
  }

  unlock(mask);
  errno = saved;
  return error;
}

/* A raw read at the end of the scratch file runs the kernel's range/import
 * checks without touching the caller's output bytes or selecting a signal.
 * https://github.com/torvalds/linux/blob/v6.12/fs/read_write.c
 */
static int validate(long number, const void *buffer, size_t count) {
  int saved = errno;
  uint64_t mask = lock();
  int error = ENOTSUP;
  if (available()) {
    long result =
        syscall(number, descriptor, buffer, count, sizeof(siginfo_t), 0L);
    error = result < 0 ? errno : result ? EIO : 0;
  }
  unlock(mask);
  errno = saved;
  return error;
}

int gw_signal_user_validate_read(void *buffer, size_t size) {
  return validate(SYS_pread64, buffer, size);
}

int gw_signal_user_validate_readv(const struct iovec *vectors, int count) {
  return validate(SYS_preadv, vectors, (unsigned int)count);
}

int gw_signal_user_copy_in(void *destination, const void *source,
                           size_t length) {
  return copy(destination, source, length, 0);
}

int gw_signal_user_copy_out(void *destination, const void *source,
                            size_t length) {
  return copy(destination, source, length, 1);
}
