#ifndef GW_GUEST_DOMAIN_SIGNALS_H
#define GW_GUEST_DOMAIN_SIGNALS_H
#include <stdint.h>
#include "signal-actions.h"

extern struct gw_signal_actions gw_guest_actions;

struct gw_guest_signal_state {
  uint64_t *logical;
  uint64_t carrier, virtual_mask;
  struct gw_signal_target *target;
  uintptr_t stack_floor; /* Nonzero when another stack owns the dispatcher. */
};

void gw_guest_signals_init(struct gw_signal_targets *, uint64_t);
int gw_guest_sigaction(int, const void *, void *, int *);
int gw_guest_signal_dispatch(int, void *, void *, struct gw_guest_signal_state);
void gw_guest_signal_deliver(const struct gw_signal_delivery *, void *,
                             struct gw_guest_signal_state);
#endif
