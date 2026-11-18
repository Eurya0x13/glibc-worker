#ifndef GW_GUEST_FD_LEASE_H
#define GW_GUEST_FD_LEASE_H
#include "signal-targets.h"

struct gw_guest_fd_lease {
  struct gw_signal_target *target;
  struct gw_guest_fd_lease *next;
  const void *scope;
  void *storage;
  size_t size;
  uintptr_t stack;
  int fd;
};

/* A lease owns imported input and can pin the original open file description
 * after all application aliases close. Pass -1 for a storage-only lease. */
int gw_guest_fd_lease_open(struct gw_guest_fd_lease **,
                           struct gw_signal_target *, int, uintptr_t);
void *gw_guest_fd_lease_reserve(struct gw_guest_fd_lease *, size_t);
void gw_guest_fd_lease_drop(struct gw_guest_fd_lease **);
void gw_guest_fd_lease_unwind(uintptr_t, uintptr_t);
void gw_guest_fd_lease_context(uintptr_t, uintptr_t, size_t);
void gw_guest_fd_lease_end(const void *);
void gw_guest_fd_lease_finish(struct gw_signal_target *);

/* Supplied by the service. A suspended bridge signal activation is distinct
 * from its caller even if the two continuations use the same native thread. */
const void *gw_guest_fd_scope(void);
#endif
