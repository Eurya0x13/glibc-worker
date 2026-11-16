#ifndef GW_EXPERIMENT_ADOPT_H
#define GW_EXPERIMENT_ADOPT_H
#include "domain.h"
#include <stdatomic.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

enum { ADOPT_ALLOC = 1, ADOPT_FREE, ADOPT_STOP = 100, ADOPT_STOPPED };

enum { ADOPT_THREADS = 8, ADOPT_BATCHES = 3, ADOPT_ROUNDS = 100 };

enum { ADOPT_RECOVERY = 200 };

struct carrier {
  struct exchange exchange;
  void *token;
  int guest_destructors;
  int tls_destructors;
  int cleanup_handlers;
  int background_ok;
  int run_status;
  int index;
  int exit_mode;
  void *recovery_stack;
  void (*recovery_entry)(void *);
  void *recovery_argument;
  int recovery_transfers;
  int recovery_completions;
};

struct adoption {
  _Atomic int request, response, ready;
  int operation, error;
  struct carrier *carrier;
  void (*entry)(void *);
  const char *dso;
  uint64_t host_signals;
  void (*note_guest_thread)(int, long, uintptr_t);
};

static inline void adopt_wake(_Atomic int *word) {
  syscall(SYS_futex, word, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
}

static inline void adopt_wait(_Atomic int *word, int value) {
  syscall(SYS_futex, word, FUTEX_WAIT_PRIVATE, value, NULL, NULL, 0);
}

void domain_start(void);
#endif
