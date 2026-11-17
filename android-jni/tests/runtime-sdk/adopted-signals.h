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
  SIGNAL_HOST_CONTEXT,
  SIGNAL_CASES
};

static inline int adopted_parent_target(int mode) {
  return mode == SIGNAL_PARENT_JUMP || mode == SIGNAL_PARENT_CONTEXT;
}

static inline int adopted_nested(int mode) {
  return (mode >= SIGNAL_NESTED && mode <= SIGNAL_NESTED_JUMP) ||
         mode == SIGNAL_NESTED_CONTEXT || adopted_parent_target(mode);
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
};

_Static_assert(offsetof(struct adopted_signal_probe, handled) == 4,
               "Java direct buffer layout");
#endif
