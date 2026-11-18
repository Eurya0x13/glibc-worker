#ifndef GW_GUEST_SIGNAL_WORKERS_H
#define GW_GUEST_SIGNAL_WORKERS_H
#include <stdint.h>
#include "signal-execution.h"

struct jni_domain_context;
int gw_guest_workers_start(int, uint64_t, uint64_t, int (*)(void));
void gw_guest_workers_unmask(void);
int gw_guest_workers_check_stop(void);
int gw_guest_workers_stop(void);
int gw_guest_workers_fault(int, void *, void *);
void gw_guest_workers_notify(void *, void *);
void gw_guest_workers_wake_error(int);
uint64_t gw_guest_virtual_mask(void);
uintptr_t gw_guest_signal_cookie(void);
struct gw_guest_fd_state *gw_guest_worker_fds(void);
int gw_guest_signal_adopt(struct jni_domain_context *);
struct gw_signal_target *gw_guest_worker_target(void);
int gw_guest_signal_retire(void);
int gw_guest_signal_parked(void *);
void gw_guest_signal_finish_parked(void *, enum gw_signal_exit);
#endif
