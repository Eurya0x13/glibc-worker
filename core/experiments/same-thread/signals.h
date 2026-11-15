#ifndef GW_EXPERIMENT_SIGNALS_H
#define GW_EXPERIMENT_SIGNALS_H
#include "adopt.h"
#include <signal.h>

/* Published before Guest entry, kept until the signal sender has stopped.
 * Fixed fixture capacity, not a proposed production registry design. */
struct signal_slot {
  uintptr_t host_tp, host_x18;
  struct exchange *exchange;
  _Atomic long tid;
  _Atomic int signals, faults, failures;
  void *signal_stack;
  stack_t old_stack;
  sigset_t inherited_mask;
  int inherited_blocked;
};

_Static_assert(offsetof(struct signal_slot, tid) == 24, "signal assembly ABI");

extern struct signal_slot signal_slots[ADOPT_THREADS];
void signal_install(void);
void signal_register(struct signal_slot *, struct exchange *);
void signal_unregister(struct signal_slot *);
void signal_send(void);
void signal_note_guest_thread(int, long, uintptr_t);
struct signal_slot *signal_acquire(struct exchange *);
void signal_release(struct signal_slot *);
void signal_guest_lifecycle(int);
struct signal_slot *signal_dynamic_lookup(long, uintptr_t);
void domain_fault(void *);
extern char domain_fault_instruction[], domain_fault_resume[];
int adoption_signal_check(struct signal_slot *);

#endif
