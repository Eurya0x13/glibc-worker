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
  SIGNAL_CASES
};

static inline int adopted_nested(int mode) {
  return mode >= SIGNAL_NESTED && mode <= SIGNAL_NESTED_JUMP;
}

static inline int adopted_guest_target(int mode) {
  return mode == SIGNAL_GUEST_JUMP_SAVED || mode == SIGNAL_GUEST_JUMP_UNSAVED ||
         mode == SIGNAL_NESTED_JUMP;
}

struct adopted_signal_probe {
  _Atomic int requested, handled;
  int mode, tid;
  _Atomic int host_returned;
};

_Static_assert(offsetof(struct adopted_signal_probe, handled) == 4,
               "Java direct buffer layout");
#endif
