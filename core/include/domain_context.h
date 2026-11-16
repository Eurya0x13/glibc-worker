#ifndef GW_DOMAIN_CONTEXT_H
#define GW_DOMAIN_CONTEXT_H
#include <stdint.h>

/* Saved stack, thread pointer, continuation and callee-saved register state.
 * Private register slots are architecture-specific. Initialize via the owning
 * runtime, never synthesize a context or change its TLS pointer in a caller.
 * Both stacks and both TLS allocations must outlive every saved continuation.
 * This low-level primitive does not establish a signal or pthread contract. */
struct domain {
  uintptr_t sp;
  uintptr_t tp;
  uintptr_t pc;
  uint64_t saved[29];
};

#ifdef __cplusplus
static_assert(sizeof(struct domain) == 256, "assembly context layout");
extern "C" {
#else
_Static_assert(sizeof(struct domain) == 256, "assembly context layout");
#endif
void domain_swap(struct domain *from, struct domain *to);
#ifdef __cplusplus
}
#endif
#endif
