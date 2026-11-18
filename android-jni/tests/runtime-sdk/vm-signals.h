#ifndef GW_SDK_VM_SIGNALS_H
#define GW_SDK_VM_SIGNALS_H
#include <stdatomic.h>
#include <stddef.h>

struct sdk_vm_signals {
  _Atomic int stage, command;
};

enum {
  SDK_VM_NATIVE_READY = 1,
  SDK_VM_JAVA_READY = 2,
  SDK_VM_JAVA_RETURNED = 3,
  SDK_VM_STACKS_RELEASED = 4
};

_Static_assert(sizeof(struct sdk_vm_signals) == 8 &&
                   offsetof(struct sdk_vm_signals, command) == 4,
               "Java direct buffer layout");
#endif
