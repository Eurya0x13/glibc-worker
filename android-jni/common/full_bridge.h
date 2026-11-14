#ifndef FULL_BRIDGE_H
#define FULL_BRIDGE_H
#include "full_protocol.h"
#include "host_bridge.h"
struct shared;
void full_host_poll(void);
void full_guest_start(struct shared *);
void full_guest_poll(void);
/* Pump controller/current-context work, then park until notified or stopping.
   Returns zero when the runtime or owner requested shutdown. */
int full_guest_wait(struct shared *);
void full_guest_shutdown(void);
#endif
