#ifndef GW_GUEST_EPOLL_H
#define GW_GUEST_EPOLL_H
#include "signal-targets.h"
#include <poll.h>
#include <sys/epoll.h>

int gw_guest_epoll_ctl(struct gw_signal_targets *, int, int, int,
                       const struct epoll_event *);
int gw_guest_epoll_reap(void);
int gw_guest_epoll_stop(void);
int gw_guest_epoll_valid(int);
int gw_guest_epoll_known(int);
int gw_guest_epoll_ready(int, const struct gw_signal_poll_sample *);
int gw_guest_epoll_events(int, struct epoll_event *, int,
                          const struct gw_signal_poll_sample *);

/* Internal nested-watch probes can become readable independently of the
 * public epoll file. Include them when planning a blocking wait. */
size_t gw_guest_epoll_probes(int, struct pollfd *, size_t);
#endif
