#ifndef GW_SDK_ADOPTED_SIGNALS_H
#define GW_SDK_ADOPTED_SIGNALS_H
#include <stdatomic.h>
#include <stddef.h>

enum {
  SIGNAL_JAVA,
  SIGNAL_POLL,
  SIGNAL_SLEEP,
  SIGNAL_HOST_JUMP,
  SIGNAL_GUEST_JUMP_SAVED,
  SIGNAL_GUEST_JUMP_UNSAVED,
  SIGNAL_NESTED,
  SIGNAL_NESTED_ALT,
  SIGNAL_NESTED_JUMP,
  SIGNAL_GUEST_CONTEXT,
  SIGNAL_NESTED_CONTEXT,
  SIGNAL_PARENT_JUMP,
  SIGNAL_PARENT_CONTEXT,
  SIGNAL_CONTEXT_EDIT,
  SIGNAL_SLEEP_ABSOLUTE,
  SIGNAL_PARENT_SWAP,
  SIGNAL_PARENT_SWAP_OVERLAP,
  SIGNAL_HOST_CONTEXT,
  SIGNAL_RETIRE_SAVED,
  SIGNAL_RETIRE_HOST_JUMP,
  SIGNAL_RETIRE_CHAIN,
  SIGNAL_CASES
};

static inline int adopted_retires_saved(int mode) {
  return mode == SIGNAL_RETIRE_SAVED || mode == SIGNAL_RETIRE_HOST_JUMP ||
         mode == SIGNAL_RETIRE_CHAIN;
}

static inline int adopted_scope_swap(int mode) {
  return mode == SIGNAL_PARENT_SWAP || adopted_retires_saved(mode);
}

static inline int adopted_parent_target(int mode) {
  return mode == SIGNAL_PARENT_JUMP || mode == SIGNAL_PARENT_CONTEXT;
}

static inline int adopted_nested(int mode) {
  return (mode >= SIGNAL_NESTED && mode <= SIGNAL_NESTED_JUMP) ||
         mode == SIGNAL_NESTED_CONTEXT || adopted_scope_swap(mode) ||
         mode == SIGNAL_PARENT_SWAP_OVERLAP || adopted_parent_target(mode);
}

static inline int adopted_activations(int mode) {
  return mode == SIGNAL_RETIRE_CHAIN ? 3 : 1 + adopted_nested(mode);
}

static inline int adopted_context(int mode) {
  return mode == SIGNAL_GUEST_CONTEXT || mode == SIGNAL_NESTED_CONTEXT;
}

static inline int adopted_guest_target(int mode) {
  return mode == SIGNAL_GUEST_JUMP_SAVED || mode == SIGNAL_GUEST_JUMP_UNSAVED ||
         mode == SIGNAL_NESTED_JUMP || adopted_context(mode);
}

struct adopted_signal_probe {
  _Atomic int requested, handled;
  int mode, tid;
  _Atomic int host_returned;
  _Atomic int parent_resumed;
  _Atomic int swap_phase, host_entries;
  _Atomic int overlap_rejected;
  _Atomic int stale_rejections;
};

enum { SIGNAL_SWAP_ROUNDS = 3 };

_Static_assert(offsetof(struct adopted_signal_probe, handled) == 4,
               "Java direct buffer layout");
#endif
