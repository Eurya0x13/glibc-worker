#ifndef GW_EXPERIMENT_DOMAIN_H
#define GW_EXPERIMENT_DOMAIN_H

#include <stddef.h>
#include <stdint.h>
#include <signal.h>

/* Private assembly ABI, deliberately outside the installed core API. */
struct domain {
  uintptr_t sp;
  uintptr_t tp;
  uintptr_t pc;
  uint64_t saved[29];
};

_Static_assert(sizeof(struct domain) == 256, "assembly context layout");

enum { DOMAIN_READY = 1, DOMAIN_CALL, DOMAIN_CALLBACK, DOMAIN_DONE };

enum { DOMAIN_ROUNDS = 10000, DOMAIN_NESTING = 8 };

/* Linux test-owned realtime signals. Avoid HotSpot SIGUSR2; installation
 * checks that both actions are unused instead of assuming signal ownership. */
enum { DOMAIN_NOTIFY_SIGNAL = 50, DOMAIN_NESTED_SIGNAL = 51 };

struct exchange {
  struct domain host, guest;
  void (*swap)(struct domain *, struct domain *);
  const char *dso;
  int state, failures, calls, callbacks, nested, entries;
  int depth, value;
  long host_tid, guest_tid;
  uintptr_t host_tls, guest_tls, dso_tls;
  uintptr_t stack_low, stack_high;
  int *allocation;
  volatile sig_atomic_t signal_count;
  volatile uintptr_t signal_tp[2];
  void (*fault)(void *);
  void *fault_page;
  int host_signal_probe;
  int host_signal_expected[3];
  volatile sig_atomic_t host_signal_delivered[3];
};

void domain_swap(struct domain *from, struct domain *to);

/* Failures are counted in shared POD storage; no foreign libc is called. */
#define CHECK(e, condition) ((e)->failures += !(condition))

typedef int (*domain_callback)(struct exchange *, int);
typedef int (*domain_probe)(struct exchange *, domain_callback);

#endif
