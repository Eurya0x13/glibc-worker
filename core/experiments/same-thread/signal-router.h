#ifndef GW_EXPERIMENT_SIGNAL_ROUTER_H
#define GW_EXPERIMENT_SIGNAL_ROUTER_H
#include <signal.h>
#include <stdint.h>

/* Host registration layer. Initialize before creating the VM or Guest owner.
 * This does not virtualize Guest libc's independently resolved sigaction. */
int signal_router_init(void);
typedef int (*signal_router_backend)(int, const struct sigaction *,
                                     struct sigaction *);
/* Explicit backend must be the bottom Host libc registration API. Late
 * integration owns registration quiescence and future import routing. */
int signal_router_init_with_backend(signal_router_backend);
int signal_router_adopt(void);
/* Only after a reviewed outer chain's bottom registration/query pointers
 * have been routed. NULL keeps the conservative standalone policy. */
int signal_router_adopt_chain(void *reviewed_chain_base);
#ifdef __ANDROID__
int signal_router_action64(int, const struct sigaction64 *,
                           struct sigaction64 *);
#endif
int signal_router_action(int, const struct sigaction *, struct sigaction *);
int signal_router_restore(void);
void signal_router_forward(int, siginfo_t *, void *, void *);
int signal_router_verify(void);
unsigned signal_router_registrations(void);
uint64_t signal_router_reserved(void);
#endif
