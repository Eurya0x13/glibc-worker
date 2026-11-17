#ifndef GW_GUEST_DOMAIN_SIGNALS_H
#define GW_GUEST_DOMAIN_SIGNALS_H
#include <stdint.h>

void gw_guest_signals_init(void);
int gw_guest_sigaction(int, const void *, void *, int *);
int gw_guest_signal_dispatch(int, void *, void *, uint64_t *, uint64_t,
                             uint64_t);
#endif
