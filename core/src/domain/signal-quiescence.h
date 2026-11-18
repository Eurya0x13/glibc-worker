#ifndef GW_DOMAIN_SIGNAL_QUIESCENCE_H
#define GW_DOMAIN_SIGNAL_QUIESCENCE_H
#include <signal.h>

void gw_signal_quiescence_ack(int, siginfo_t *, void *);
int gw_signal_quiescence_check(int number, unsigned timeout_ms);
#endif
