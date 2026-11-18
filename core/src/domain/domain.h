#ifndef GW_DOMAIN_INTERNAL_H
#define GW_DOMAIN_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <signal.h>

#include "domain_context.h"

enum { DOMAIN_READY = 1, DOMAIN_CALL, DOMAIN_CALLBACK, DOMAIN_DONE };

enum { DOMAIN_ROUNDS = 10000, DOMAIN_NESTING = 8 };

/* Linux test-owned realtime signals. Avoid HotSpot SIGUSR2; installation
 * checks that both actions are unused instead of assuming signal ownership. */
enum { DOMAIN_NOTIFY_SIGNAL = 50, DOMAIN_NESTED_SIGNAL = 51 };

struct gw_signal_target;

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
  /* Signal snapshots must not overwrite the cooperative switch target:
   * a signal can arrive after its TP changed but before its fenv was loaded. */
  uint64_t interrupted_control[3];
  int host_signal_expected[3];
  volatile sig_atomic_t host_signal_delivered[3];
  uint64_t host_execution_mask, interrupted_guest_mask;
  int host_mask_valid;
  int (*guest_signal)(int, void *, void *);
  stack_t host_altstack, interrupted_guest_altstack;
  /* Reserved physical bits for Guest execution can differ from Host's logical
   * mask when fault delivery uses deferred Host notifications. */
  uint64_t guest_execution_mask;
  struct gw_signal_target *guest_signal_target;
};

/* Private signal-entry call frame; both architecture stubs use these offsets.
 * The kernel frame and registration remain live across a Guest nonlocal exit.
 */
struct domain_signal_call {
  int (*entry)(int, void *, void *);
  uintptr_t tp, x18;
  uint64_t control[2];
  int number;
  void *info, *context;
};

_Static_assert(offsetof(struct domain_signal_call, number) == 40,
               "signal-call assembly ABI");
_Static_assert(offsetof(struct domain_signal_call, context) == 56,
               "signal-call assembly ABI");
int domain_guest_signal_call(const struct domain_signal_call *);

/* Failures are counted in shared POD storage; no foreign libc is called. */
#define CHECK(e, condition) ((e)->failures += !(condition))

typedef int (*domain_callback)(struct exchange *, int);
typedef int (*domain_probe)(struct exchange *, domain_callback);

#endif
