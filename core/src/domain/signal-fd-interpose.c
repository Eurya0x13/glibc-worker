#define _GNU_SOURCE
#include "signals.h"
#include "signal-router.h"
#include "signal-consume.h"
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
  return 0;
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
static int decode(struct signalfd_siginfo *record) {
  siginfo_t information;
  int original =
      gw_signal_consume_notification(record->ssi_signo, &information);
  if (original < 0) {
    return 1;
  }
  if (!original) {
    return 0;
  }
  memset(record, 0, sizeof *record);
  record->ssi_signo = original;
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
  return 1;
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
      } else {
        memcpy(bytes, part, length);
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

static ssize_t translate(const struct iovec *vectors, int count, ssize_t size) {
  _Static_assert(sizeof(struct signalfd_siginfo) == 128, "Linux signalfd ABI");
  struct vector_cursor input = {.vectors = vectors, .count = count},
                       output = input;
  ssize_t written = 0;
  int saved_errno = errno;
  uint64_t previous = gw_nonlocal_mask_block();
  for (ssize_t offset = 0; offset < size; offset += 128) {
    struct signalfd_siginfo record;
    vector_copy(&input, &record, 0);
    if (decode(&record)) {
      vector_copy(&output, &record, 1);
      written += 128;
    }
  }
  gw_nonlocal_mask_restore(previous);
  errno = saved_errno;
  return written;
}

struct read_arguments {
  int descriptor, count, vector;
  void *buffer;
  size_t size;
  const struct iovec *vectors;
  void *function;
  gw_signal_fd_reader backend;
  void *cookie;
  ssize_t native_result;
};

static long invoke_read(void *argument) {
  struct read_arguments *args = argument;
  if (args->backend) {
    args->native_result = args->backend(args->cookie, args->buffer, args->size);
    return args->native_result;
  }

  if (args->vector) {
    typedef ssize_t (*function_type)(int, const struct iovec *, int);
    args->native_result = ((function_type)args->function)(
        args->descriptor, args->vectors, args->count);
    return args->native_result;
  }

  typedef ssize_t (*function_type)(int, void *, size_t);
  args->native_result = ((function_type)args->function)(
      args->descriptor, args->buffer, args->size);
  return args->native_result;
}

static long decode_read(void *argument, long result) {
  struct read_arguments *args = argument;
  if (result <= 0) {
    return result;
  }

  if (args->vector) {
    return translate(args->vectors, args->count, result);
  }

  struct iovec vector = {.iov_base = args->buffer, .iov_len = result};
  return translate(&vector, 1, result);
}

ssize_t gw_signal_fd_read_backend(int descriptor, void *buffer, size_t size,
                                  gw_signal_fd_reader backend, void *cookie) {
  int translated = translated_descriptor(descriptor);
  if (translated < 0) {
    return -1;
  }
  if (!translated) {
    return backend(cookie, buffer, size);
  }

  for (;;) {
    signal_host_prune_notifications();
    struct read_arguments args = {.descriptor = descriptor,
                                  .buffer = buffer,
                                  .size = size,
                                  .backend = backend,
                                  .cookie = cookie};
    ssize_t result = gw_signal_consume_call(invoke_read, decode_read, &args);
    if (result || args.native_result == 0) {
      return result;
    }
  }
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
  for (;;) {
    signal_host_prune_notifications();
    struct read_arguments args = {.descriptor = descriptor,
                                  .vectors = vectors,
                                  .count = count,
                                  .vector = 1,
                                  .function = (void *)function};
    ssize_t result = gw_signal_consume_call(invoke_read, decode_read, &args);
    if (result || args.native_result == 0) {
      return result;
    }
  }
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
