#ifndef GW_SIGNAL_SELECT_H
#define GW_SIGNAL_SELECT_H
#include "signal-deferred.h"
#include <sys/uio.h>
#include <sys/signalfd.h>

/* Bounded, nonblocking dequeue transaction. No caller handler or cancellation
 * runs between kernel selection and logical commit. */
struct gw_signal_selection {
  struct gw_signal_deferred *deferred;
  uint64_t mask;
  int cancellation, descriptor;
};

void gw_signal_selection_begin(struct gw_signal_selection *);
int gw_signal_selection_end(struct gw_signal_selection *);
uint64_t gw_signal_selection_pending(struct gw_signal_selection *, uint64_t);
int gw_signal_selection_take(struct gw_signal_selection *, uint64_t,
                             siginfo_t *);
int gw_signal_selection_record(struct gw_signal_selection *, uint64_t,
                               struct signalfd_siginfo *, siginfo_t *);
int gw_signal_selection_mask(struct gw_signal_selection *, uint64_t);
ssize_t gw_signal_selection_copy(struct gw_signal_selection *,
                                 const struct iovec *, int);
int gw_signal_selection_wait_copy(struct gw_signal_selection *, siginfo_t *,
                                  const siginfo_t *);
/* Wait without consuming anything. Its stable record can survive a Host
 * setcontext/longjmp; no pointer to the abandoned call stack is published. */
int gw_signal_selection_sleep(int, uint64_t, const struct timespec *, int);
void *gw_signal_selection_interrupt(void *, void *);
int signal_select_prepare(void);
#endif
