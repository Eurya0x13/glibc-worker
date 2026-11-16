#ifndef GW_SIGNAL_CONSUME_H
#define GW_SIGNAL_CONSUME_H
#include <signal.h>
#include <stdint.h>

/* These callbacks always execute with Host TLS. invoke retains the owning
 * libc's blocking/cancellation entry; decode runs with catchable signals
 * blocked, before the consumption boundary is retired. */
typedef long (*gw_signal_consume_fn)(void *);
typedef long (*gw_signal_decode_fn)(void *, long);
long gw_signal_consume_call(gw_signal_consume_fn, gw_signal_decode_fn, void *);
int gw_signal_consume_notification(int, siginfo_t *);
int gw_signal_consume_busy(void);

struct gw_signal_consumption;
/* Signal entry already blocks catchable signals and restores Host TP. Pause
 * only an in-flight native consumer; resume only after normal handler return.
 * A nonlocal exit retires it through its ordinary native unwind boundary. */
struct gw_signal_consumption *gw_signal_consume_interrupt(int);
void gw_signal_consume_resume(struct gw_signal_consumption *);
#endif
