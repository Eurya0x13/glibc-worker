#ifndef GW_GUEST_SIGNAL_WORKERS_H
#define GW_GUEST_SIGNAL_WORKERS_H
#include <stdint.h>

int gw_guest_workers_start(int, uint64_t, int (*)(void));
void gw_guest_workers_unmask(void);
int gw_guest_workers_stop(void);
int gw_guest_workers_fault(int, void *, void *);
void gw_guest_workers_notify(void *, void *);
void gw_guest_workers_wake_error(int);
#endif
