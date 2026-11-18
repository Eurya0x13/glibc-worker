#ifndef GW_JNI_DOMAIN_UNWIND_H
#define GW_JNI_DOMAIN_UNWIND_H
#include <stdint.h>
struct fb_call;

struct gw_domain_native_checkpoint {
  void *buffer;
  struct fb_call *call;
  struct gw_domain_native_checkpoint *previous;
  uint64_t generation;
};

/* These are deterministic test injection points, not runtime configuration.
 * Test-enabled SDKs require the test executable to supply the callback. */
enum gw_domain_unwind_point {
  GW_UNWIND_TRAMPOLINE_READY = 1,
  GW_UNWIND_COMMAND_PUBLISHED,
  GW_UNWIND_COMMAND_FINISHED,
  GW_UNWIND_TRAMPOLINE_CLEANED,
  GW_UNWIND_RPC_READY,
  GW_UNWIND_RPC_RESULT_OWNED,
  GW_UNWIND_RPC_RESULT_DISCARDED,
};

#ifdef GW_DOMAIN_NONLOCAL_TEST
extern void gw_domain_nonlocal_test_point(int) __attribute__((weak));
extern void gw_domain_nonlocal_test_result(int, int, uintptr_t, uintptr_t)
    __attribute__((weak));
#define GW_UNWIND_POINT(point)              \
  do {                                      \
    if (gw_domain_nonlocal_test_point) {    \
      gw_domain_nonlocal_test_point(point); \
    }                                       \
  } while (0)
#else
#define GW_UNWIND_POINT(point) ((void)0)
#endif
#endif
