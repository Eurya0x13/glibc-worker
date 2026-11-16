#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdatomic.h>
#include <unistd.h>

/* Test-only next-link backend. Invoke the actual libc cancellation entry, then
 * inject before its result reaches the public wrapper. No production read or
 * close is replaced by a raw syscall, including in the cancellation control. */
static _Atomic(void *) native;
extern void signal_fd_native_close_observe(int) __attribute__((weak));

int close(int descriptor) {
  typedef int (*close_fn)(int);
  close_fn function = atomic_load_explicit(&native, memory_order_acquire);
  if (!function) {
    function = (close_fn)dlsym(RTLD_NEXT, "close");
    if (!function) {
      _exit(127);
    }
    atomic_store_explicit(&native, (void *)function, memory_order_release);
  }
  int result = function(descriptor), error = errno;
  if (!result && signal_fd_native_close_observe) {
    signal_fd_native_close_observe(descriptor);
  }
  errno = error;
  return result;
}
