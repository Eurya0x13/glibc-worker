#ifndef GW_LIBC_CONTEXT_EVENTS_H
#define GW_LIBC_CONTEXT_EVENTS_H
#include <ucontext.h>

enum gw_context_operation {
  GW_CONTEXT_CAPTURE,
  GW_CONTEXT_RESTORE,
  GW_CONTEXT_SWAP,
  GW_CONTEXT_CONSTRUCT
};

typedef int (*gw_libc_context_hook)(enum gw_context_operation, ucontext_t *,
                                    const ucontext_t *);
#endif
