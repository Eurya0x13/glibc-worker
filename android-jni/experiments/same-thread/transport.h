#pragma once
/* Fixture protocol, never an installed bridge ABI. Production JNI slots and
 * dispatchers are used unchanged; only the optional handoff hooks differ. */
#include "domain.h"
#include "protocol.h"
#include "full_bridge.h"
#include <stdatomic.h>
#include <string.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

/* Selection for adoption-enabled entrypoints only. Normal installed SDK builds
 * have their own lifecycle and do not consume this experiment's environment.
 * Missing selection means same-thread; worker is an explicit compatibility
 * choice. Reject typos instead of quietly changing the execution contract. */
static inline int jni_domain_select(const char *mode) {
  if (!mode || !strcmp(mode, "domain")) {
    return 1;
  }
  if (!strcmp(mode, "worker")) {
    return 0;
  }
  return -1;
}

enum { JNI_DOMAIN_ALLOC = 1, JNI_DOMAIN_FREE, JNI_DOMAIN_STOP };

struct jni_domain_context {
  struct exchange exchange;
  void *token;
  _Atomic int stop, ready;
  int id;
  int domain;
  int fail_allocate;
};

struct jni_domain_session {
  struct shared shared;
  struct jni_domain_context *context;
  _Atomic int request, response, ready;
  int operation, error;
  int domain;
  uint64_t reserved_signals;
  void (*entry)(void *);
  void (*note_guest)(int, long, uintptr_t);
  void (*guest_thread)(int);
};

static inline void wake_word(_Atomic int *p) {
  syscall(SYS_futex, p, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
}

static inline void wait_word(_Atomic int *p, int v) {
  syscall(SYS_futex, p, FUTEX_WAIT_PRIVATE, v, NULL, NULL, 0);
}
