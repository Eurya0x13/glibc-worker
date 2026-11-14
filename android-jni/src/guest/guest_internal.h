#ifndef GW_GUEST_INTERNAL_H
#define GW_GUEST_INTERNAL_H
#include <jni.h>
#include "full_bridge.h"
#pragma GCC visibility push(hidden)
/* GNU thread-local state, never accessed on a bionic thread. */
extern struct full_bridge *G;
extern __thread int gid, gdepth;
extern __thread uint64_t gmodule;
extern __thread JNIEnv fg_env;
extern JavaVM fg_vm;
void fg_execute(struct fb_call *);
#pragma GCC visibility pop
#endif
