#ifndef GW_SIGNAL_SELECT_H
#define GW_SIGNAL_SELECT_H
#include "signal-deferred.h"
#include <sys/uio.h>
#include <sys/signalfd.h>
#include <time.h>

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
int gw_signal_selection_sleep(int, uint64_t, const struct timespec *, int,
                              void (*)(void *), void *);
int gw_signal_selection_interrupt(void *, void *, uint64_t *);
/* Host TP is restored and catchable signals are blocked. Hidden notifications
 * do not become Host handler interruptions. A later real Host delivery wins. */
int gw_signal_selection_hidden(void *, uint64_t *);
void gw_signal_wait_host_delivery(int restart);
/* Begin leaves delivery blocked until the caller's raw atomic mask-wait.
 * End restores the caller mask/valid handler edits and retires the stable row.
 */
long gw_signal_wait_syscall(void *, long, long, long, long, long, long, long);
/* Native non-mask syscalls use an identified unmask/entry window, then block
 * delivery again before returning to C. Kernel timer/copyout rules stay native.
 * Begin/end and retry use the same stable row as atomic mask waits. */
long gw_signal_native_wait_syscall(void *, long, long, long, long, long, long,
                                   long);
int gw_signal_native_wait_attempted(void *);
void gw_signal_native_wait_clock(void *, clockid_t);
/* Interval containing hidden work only, ending before a nested visible Host
 * handler when present. The kernel's native remainder excludes handler work. */
int gw_signal_native_wait_interval(void *, struct timespec *,
                                   struct timespec *);
/* The caller owns a compiler exception cleanup, with delivery blocked while
 * entering/leaving these phases. Handler entry restores the prior mode before
 * exposing user code; no libc cleanup buffer remains on an abandoned stack. */
void gw_signal_native_wait_cancel_begin(void *);
void gw_signal_native_wait_cancel_end(void *);
void gw_signal_native_wait_rearm(void *);
__int128 gw_signal_native_wait_host_shift(void *);
int gw_signal_native_wait_retry(void *, int result, int error);
int gw_signal_native_read_retry(void *, int result, int error, int cancellable);
int gw_signal_wait_result(long);
void *gw_signal_mask_wait_begin(uint64_t);
void *gw_signal_mask_wait_current_begin(void);
uint64_t gw_signal_mask_wait_execution(void *);
int gw_signal_mask_wait_end(void *, int result, int error);
void gw_signal_mask_wait_cancel_check(void *);
/* A true result republishes the stable row and leaves delivery blocked for
 * another atomic syscall. The caller retains the original timeout budget. */
int gw_signal_mask_wait_retry(void *, int result, int error);
int signal_select_prepare(void);
#endif
