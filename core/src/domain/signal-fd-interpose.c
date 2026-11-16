#define _GNU_SOURCE
#include "signals.h"
#include "signal-router.h"
#include "signal-select.h"
#include "signal-fd-kernel.h"
#include "signal-fd-adapter.h"
#include "nonlocal-mask.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/uio.h>
#include <sys/mman.h>

/* The opt-in channel identifies actual kernel objects, not a process-global
 * numeric FD cache. Aliases/imports and private FD tables therefore require no
 * userspace lifetime shadow. Raw reads/private libc handles still bypass this
 * explicit bootstrap interposition. Owners serialize replacement with reads.
 */
enum backend_index {
  B_SIGNALFD,
  B_READ,
  B_READV,
  B_CLOSE,
  B_DUP,
  B_DUP2,
  B_DUP3,
  B_FCNTL,
  B_COUNT
};

static _Atomic(void *) natives[B_COUNT];
extern void signal_fd_close_observe(int) __attribute__((weak));

static void *resolve(enum backend_index index, const char *name) {
  void *result = atomic_load_explicit(&natives[index], memory_order_acquire);
  if (!result) {
    result = dlsym(RTLD_NEXT, name);
    if (result) {
      atomic_store_explicit(&natives[index], result, memory_order_release);
    } else {
      errno = ENOSYS;
    }
  }
  return result;
}

#ifdef __ANDROID__
#define SIGNALFD_NATIVE "signalfd64"
#else
#define SIGNALFD_NATIVE "signalfd"
#endif

int signal_fd_prepare(void) {
  const char *names[B_COUNT] = {SIGNALFD_NATIVE, "read", "readv", "close",
                                "dup",           "dup2", "dup3",  "fcntl"};
  for (int i = 0; i < B_COUNT; ++i) {
    if (!resolve(i, names[i])) {
      return ENOSYS;
    }
  }
  return signal_select_prepare();
}

int signal_fd_channel_ready(void) {
  return gw_signal_fd_inventory(0);
}

int signal_fd_channel_busy(void) {
  const int *notifications = signal_router_fault_notifications();
  if (!notifications) {
    return 0;
  }
  uint64_t bits = 0;
  for (int i = 0; i < GW_SIGNAL_FAULT_LANES; ++i) {
    bits |= UINT64_C(1) << (notifications[i] - 1);
  }
  return gw_signal_fd_inventory(bits);
}

static int translated_descriptor(int descriptor) {
  const int *notifications = signal_router_fault_notifications();
  if (!notifications) {
    return 0;
  }
  uint64_t mask;
  int result = gw_signal_fd_kernel(descriptor, &mask);
  if (result <= 0) {
    return result;
  }
  for (int i = 0; i < GW_SIGNAL_FAULT_LANES; ++i) {
    if (mask & (UINT64_C(1) << (notifications[i] - 1))) {
      return 1;
    }
  }
  return 0;
}

int signalfd(int descriptor, const sigset_t *mask, int flags) {
  typedef int (*function_type)(int, const sigset_t *, int);
  function_type function = (function_type)resolve(B_SIGNALFD, SIGNALFD_NATIVE);
  if (!function) {
    return -1;
  }
  sigset_t physical;
  if (signal_router_fault_notifications()) {
    uint64_t bits;
    memcpy(&bits, mask, sizeof bits);
    bits = signal_host_wait_mask(bits);
    sigemptyset(&physical);
    memcpy(&physical, &bits, sizeof bits);
    mask = &physical;
  }
  return function(descriptor, mask, flags);
}

#ifdef __ANDROID__
int signalfd64(int descriptor, const sigset64_t *mask, int flags) {
  _Static_assert(sizeof(sigset64_t) == sizeof(sigset_t), "LP64 signal mask");
  return signalfd(descriptor, (const sigset_t *)mask, flags);
}
#endif

int close(int descriptor) {
  typedef int (*function_type)(int);
  function_type function = (function_type)resolve(B_CLOSE, "close");
  if (!function) {
    return -1;
  }
  /* No metadata transaction to abandon. Keep libc's real cancellation point
   * and never close a reused number from cleanup. The weak hook is
   * fixture-only.
   * https://github.com/bminor/glibc/blob/glibc-2.41/sysdeps/unix/sysv/linux/close.c
   */
  int result = function(descriptor), error = errno;
  if (signal_fd_close_observe) {
    signal_fd_close_observe(descriptor);
  }
  errno = error;
  return result;
}

int dup(int descriptor) {
  typedef int (*function_type)(int);
  function_type function = (function_type)resolve(B_DUP, "dup");
  return function ? function(descriptor) : -1;
}

int dup2(int source, int target) {
  typedef int (*function_type)(int, int);
  function_type function = (function_type)resolve(B_DUP2, "dup2");
  return function ? function(source, target) : -1;
}

int dup3(int source, int target, int flags) {
  typedef int (*function_type)(int, int, int);
  function_type function = (function_type)resolve(B_DUP3, "dup3");
  return function ? function(source, target, flags) : -1;
}

/* Assembly forwards an optional vararg without reading an absent C argument. */
int signal_fd_fcntl(int descriptor, int command, uintptr_t argument) {
  typedef int (*function_type)(int, int, ...);
  function_type function = (function_type)resolve(B_FCNTL, "fcntl");
  return function ? function(descriptor, command, argument) : -1;
}

/* Linux packs precisely 128 bytes per record. Only our six private numbers
 * require conversion; ordinary kernel records pass through byte-for-byte.
 * Match signalfd_copyinfo's layout, rather than copying libc's siginfo union.
 * https://github.com/torvalds/linux/blob/v6.12/fs/signalfd.c
 */
static void encode(struct signalfd_siginfo *record, const siginfo_t *source) {
  const siginfo_t information = *source;
  memset(record, 0, sizeof *record);
  record->ssi_signo = information.si_signo;
  record->ssi_errno = information.si_errno;
  record->ssi_code = information.si_code;
  if (information.si_code == SI_TIMER) {
    record->ssi_tid = information.si_timerid;
    record->ssi_overrun = information.si_overrun;
    record->ssi_ptr = (uintptr_t)information.si_value.sival_ptr;
    record->ssi_int = information.si_value.sival_int;
  } else if (information.si_code == SI_SIGIO) {
    record->ssi_band = information.si_band;
    record->ssi_fd = information.si_fd;
  } else {
    record->ssi_pid = information.si_pid;
    record->ssi_uid = information.si_uid;
    if (information.si_code != SI_USER && information.si_code != SI_TKILL) {
      record->ssi_ptr = (uintptr_t)information.si_value.sival_ptr;
      record->ssi_int = information.si_value.sival_int;
    }
  }
}

struct vector_cursor {
  const struct iovec *vectors;
  int count, index;
  size_t offset;
};

static void vector_copy(struct vector_cursor *cursor, void *record, int write) {
  unsigned char *bytes = record;
  size_t left = sizeof(struct signalfd_siginfo);
  while (left) {
    if (cursor->index >= cursor->count) {
      _exit(
          105); /* The successful native read supplied an inconsistent size. */
    }
    const struct iovec *vector = &cursor->vectors[cursor->index];
    size_t length = vector->iov_len - cursor->offset;
    if (length > left) {
      length = left;
    }
    if (length) {
      unsigned char *part = (unsigned char *)vector->iov_base + cursor->offset;
      if (write) {
        memcpy(part, bytes, length);
      }
      bytes += length;
      left -= length;
      cursor->offset += length;
    }
    if (cursor->offset == vector->iov_len) {
      ++cursor->index;
      cursor->offset = 0;
    }
  }
}

extern void gw_signal_consume_test_point(void) __attribute__((weak));

/* The kernel validated the original iovec array before this walk. Keep the
 * running kernel's per-vector rules by validating each one-record prefix on
 * an empty native signalfd before selecting a signal. IOV_MAX is a kernel ABI
 * validation bound, not a limit on runtime objects. Zero vectors carry no data.
 */
static int prefix(struct vector_cursor cursor, struct iovec *vectors) {
  size_t left = sizeof(struct signalfd_siginfo);
  int count = 0;
  while (left && cursor.index < cursor.count) {
    const struct iovec *source = &cursor.vectors[cursor.index++];
    size_t length = source->iov_len - cursor.offset;
    if (length) {
      if (length > left) {
        length = left;
      }
      vectors[count++] = (struct iovec){
          .iov_base = (void *)((uintptr_t)source->iov_base + cursor.offset),
          .iov_len = length};
      left -= length;
    }
    cursor.offset = 0;
  }
  return left ? 0 : count;
}

static ssize_t consume_records(struct gw_signal_selection *state,
                               uint64_t wanted, const struct iovec *vectors,
                               int count, int scalar) {
  uint64_t empty = 0;
  if (gw_signal_selection_mask(state, empty) < 0) {
    return -1;
  }

  /* Let the kernel validate count, total size and iovec memory before any
   * userspace dereference or logical dequeue. EAGAIN means valid, no pending.
   */
  ssize_t result = scalar
                       ? syscall(SYS_read, state->descriptor,
                                 vectors[0].iov_base, vectors[0].iov_len)
                       : syscall(SYS_readv, state->descriptor, vectors, count);
  if (result != -1 || errno != EAGAIN) {
    return result;
  }

  ssize_t written = 0;
  /* VFS caps one read/readv at MAX_RW_COUNT. Validation above keeps the
   * distinct scalar/empty-vector errors; do not reinterpret read(0) as readv.
   * https://github.com/torvalds/linux/blob/v6.12/fs/read_write.c
   * https://github.com/torvalds/linux/blob/v6.12/include/linux/fs.h
   */
  long page = sysconf(_SC_PAGESIZE);
  if (page <= 0) {
    errno = ENOTSUP;
    return -1;
  }
  ssize_t limit = INT_MAX & ~(page - 1);

  /* Native readv imports iovec metadata before copying output. A valid output
   * buffer can overlap that array; keep our own snapshot too. Large arrays
   * allocate only after native validation and before consuming anything. */
  struct iovec fast[8], *snapshot = fast;
  size_t snapshot_size = (size_t)count * sizeof *snapshot;
  if (count > 8) {
    snapshot = mmap(NULL, snapshot_size, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (snapshot == MAP_FAILED) {
      return -1;
    }
  }
  memcpy(snapshot, vectors, snapshot_size);
  struct vector_cursor cursor = {.vectors = snapshot, .count = count};

  while (written + 128 <= limit) {
    struct iovec record_vectors[128];
    int record_count = prefix(cursor, record_vectors);
    if (!record_count) {
      break;
    }

    if (gw_signal_selection_mask(state, empty) < 0) {
      result = -1;
      break;
    }
    result =
        syscall(SYS_readv, state->descriptor, record_vectors, record_count);
    if (result != -1 || errno != EAGAIN) {
      break;
    }

    siginfo_t information;
    struct signalfd_siginfo record;
    int staged = gw_signal_selection_pending(state, wanted) != 0;
    if (staged) {
      int original =
          gw_signal_selection_record(state, wanted, &record, &information);
      if (original < 0) {
        result = -1;
        break;
      }
      if (original) {
        encode(&record, &information);
      }
      result = gw_signal_selection_copy(state, record_vectors, record_count);
    } else {
      if (gw_signal_selection_mask(state, signal_host_wait_mask(wanted)) < 0) {
        result = -1;
        break;
      }
      result =
          syscall(SYS_readv, state->descriptor, record_vectors, record_count);
    }
    if (result < 0) {
      break;
    }
    if (result != sizeof(struct signalfd_siginfo)) {
      errno = signal_router_quarantine();
      result = -1;
      break;
    }

    vector_copy(&cursor, &record, staged);
    written += sizeof record;
  }
  int error = errno;
  if (snapshot != fast) {
    munmap(snapshot, snapshot_size);
  }
  errno = error;
  return written ? written : result;
}

static void cancellation_probe(gw_signal_fd_reader backend, void *cookie) {
  int error = errno;
  backend(cookie, NULL, 0);
  errno = error;
}

struct reader_probe {
  gw_signal_fd_reader function;
  void *cookie;
};

static void probe_reader(void *argument) {
  struct reader_probe *probe = argument;
  cancellation_probe(probe->function, probe->cookie);
}

static ssize_t consuming_read(int descriptor, const struct iovec *vectors,
                              int count, gw_signal_fd_reader backend,
                              void *cookie, int scalar) {
  int saved_errno = errno;
  struct reader_probe probe = {backend, cookie};
  for (;;) {
    cancellation_probe(backend, cookie);
    struct gw_signal_selection state;
    gw_signal_selection_begin(&state);
    uint64_t physical;
    int kind = gw_signal_fd_kernel(descriptor, &physical);
    ssize_t result = -1;
    if (kind == 1 && state.descriptor >= 0) {
      result = consume_records(&state, signal_host_logical_mask(physical),
                               vectors, count, scalar);
    } else {
      errno = kind < 0 ? errno : ENOTSUP;
    }
    int error = errno;
    int consumer = state.descriptor;
    if (gw_signal_selection_end(&state)) {
      errno = EIO;
      return -1;
    }

    if (result > 0 || (result < 0 && error != EAGAIN)) {
      if (gw_signal_consume_test_point) {
        gw_signal_consume_test_point();
      }
    }
    cancellation_probe(backend, cookie);
    if (result >= 0 || error != EAGAIN) {
      errno = result < 0 ? error : saved_errno;
      return result;
    }

    int flags = syscall(SYS_fcntl, descriptor, F_GETFL);
    if (flags < 0) {
      return -1;
    }
    if (flags & O_NONBLOCK) {
      errno = EAGAIN;
      return -1;
    }

    int ready =
        gw_signal_selection_sleep(consumer, signal_host_logical_mask(physical),
                                  NULL, 1, probe_reader, &probe);
    error = errno;
    cancellation_probe(backend, cookie);
    if (ready < 0) {
      errno = error;
      return -1;
    }
  }
}

ssize_t gw_signal_fd_read_backend(int descriptor, void *buffer, size_t size,
                                  gw_signal_fd_reader backend, void *cookie) {
  int translated = translated_descriptor(descriptor);
  if (translated < 0) {
    return -1;
  }
  if (!translated || signal_host_consumer_fd() < 0) {
    return backend(cookie, buffer, size);
  }

  struct iovec vector = {.iov_base = buffer, .iov_len = size};
  return consuming_read(descriptor, &vector, 1, backend, cookie, 1);
}

struct native_read {
  int descriptor;
  ssize_t (*function)(int, void *, size_t);
};

static ssize_t descriptor_read(void *cookie, void *buffer, size_t size) {
  struct native_read *native = cookie;
  return native->function(native->descriptor, buffer, size);
}

static ssize_t read_bytes(int descriptor, void *buffer, size_t size) {
  struct native_read native = {.descriptor = descriptor,
                               .function = (void *)resolve(B_READ, "read")};

  return native.function ? gw_signal_fd_read_backend(descriptor, buffer, size,
                                                     descriptor_read, &native)
                         : -1;
}

#ifdef __ANDROID__
static _Atomic(void *) probe_native;
static _Atomic int probe_descriptor = -1, probe_observed;

static ssize_t probe_stdio_read(int descriptor, void *buffer, size_t size) {
  if (descriptor ==
      atomic_load_explicit(&probe_descriptor, memory_order_relaxed)) {
    atomic_store_explicit(&probe_observed, 1, memory_order_relaxed);
  }

  typedef ssize_t (*function_type)(int, void *, size_t);
  function_type function =
      (void *)atomic_load_explicit(&probe_native, memory_order_acquire);
  return function(descriptor, buffer, size);
}

int gw_signal_fd_probe_stdio(void) {
  void *native = resolve(B_READ, "read");
  if (!native) {
    return ENOTSUP;
  }

  int descriptors[2];
  if (pipe2(descriptors, O_CLOEXEC)) {
    return errno;
  }

  FILE *stream = fdopen(descriptors[0], "r");
  if (!stream) {
    int error = errno;
    close(descriptors[0]);
    close(descriptors[1]);
    return error;
  }

  char expected_byte = 'x', observed_byte = 0;
  if (write(descriptors[1], &expected_byte, 1) != 1) {
    int error = errno;
    fclose(stream);
    close(descriptors[1]);
    return error ? error : EIO;
  }
  close(descriptors[1]);

  /* Observe actual native fread -> public read dispatch. Temporarily replace
   * only our cached bottom backend, with stdio users quiescent at bootstrap.
   * Once restored there is no probe branch/load on normal read calls. A late
   * in-flight probe callback is safe: its native target remains pinned.
   * https://github.com/aosp-mirror/platform_bionic/blob/main/libc/stdio/stdio.cpp
   */
  atomic_store_explicit(&probe_native, native, memory_order_release);
  atomic_store_explicit(&probe_observed, 0, memory_order_relaxed);
  atomic_store_explicit(&probe_descriptor, descriptors[0],
                        memory_order_relaxed);

  void *expected = native;
  if (!atomic_compare_exchange_strong_explicit(
          &natives[B_READ], &expected, (void *)probe_stdio_read,
          memory_order_acq_rel, memory_order_acquire)) {
    atomic_store_explicit(&probe_descriptor, -1, memory_order_relaxed);
    fclose(stream);
    return EBUSY;
  }

  size_t received = fread(&observed_byte, 1, 1, stream);
  int observed = atomic_load_explicit(&probe_observed, memory_order_relaxed);

  expected = (void *)probe_stdio_read;
  int restored = atomic_compare_exchange_strong_explicit(
      &natives[B_READ], &expected, native, memory_order_acq_rel,
      memory_order_acquire);
  atomic_store_explicit(&probe_descriptor, -1, memory_order_relaxed);
  fclose(stream);

  if (!restored) {
    return EBUSY;
  }

  return received == 1 && observed_byte == expected_byte && observed ? 0
                                                                     : ENOTSUP;
}
#endif

ssize_t read(int descriptor, void *buffer, size_t size) {
  return read_bytes(descriptor, buffer, size);
}

ssize_t readv(int descriptor, const struct iovec *vectors, int count) {
  typedef ssize_t (*function_type)(int, const struct iovec *, int);
  function_type function = (function_type)resolve(B_READV, "readv");
  if (!function) {
    return -1;
  }
  int translated = translated_descriptor(descriptor);
  if (translated < 0) {
    return -1;
  }
  if (!translated) {
    return function(descriptor, vectors, count);
  }
  if (signal_host_consumer_fd() < 0) {
    return function(descriptor, vectors, count);
  }
  struct native_read native = {.descriptor = descriptor,
                               .function = (void *)resolve(B_READ, "read")};
  return consuming_read(descriptor, vectors, count, descriptor_read, &native,
                        0);
}

ssize_t __read_chk(int descriptor, void *buffer, size_t size,
                   size_t available) {
  if (size > available || size > SSIZE_MAX) {
    typedef ssize_t (*function_type)(int, void *, size_t, size_t);
    function_type function = (function_type)dlsym(RTLD_NEXT, "__read_chk");
    if (!function) {
      _exit(105);
    }
    return function(descriptor, buffer, size, available);
  }
  /* Avoid Bionic's inline fortified read, which calls __read_chk again. */
  return read_bytes(descriptor, buffer, size);
}
