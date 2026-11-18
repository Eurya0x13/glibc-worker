#ifndef GW_LIBC_CONTEXT_EVENTS_H
#define GW_LIBC_CONTEXT_EVENTS_H
#include <stdint.h>
#include <ucontext.h>

enum gw_context_operation {
  GW_CONTEXT_CAPTURE,
  GW_CONTEXT_RESTORE,
  GW_CONTEXT_SWAP,
  GW_CONTEXT_CONSTRUCT,
  GW_CONTEXT_COMMIT
};

/* The hook has prepared a scope switch within the owning libc. Keep
 * delivery masked until the native restore has installed the target stack. */
enum { GW_CONTEXT_DEFER_MASK = -1 };

typedef int (*gw_libc_context_hook)(enum gw_context_operation, ucontext_t *,
                                    const ucontext_t *, const uint64_t *);
#endif
