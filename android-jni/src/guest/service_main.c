#define _GNU_SOURCE
#include "protocol.h"
#include "full_bridge.h"
#include <stdio.h>
#include <stdlib.h>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>

/* GNU service entry for an existing Host VM. No fixtures or JVM creation. */
int main(int argc, char **argv) {
  if (argc != 2) {
    syscall(SYS_exit, 91);
  }
  struct shared *s = (void *)(uintptr_t)strtoull(argv[1], NULL, 16);
  if (!s || s->magic != MAGIC) {
    syscall(SYS_exit, 92);
  }

  full_guest_start(s);
  s->guest_pid = getpid();
  s->guest_tid = (int)syscall(SYS_gettid);
  s->guest_tp = (uintptr_t)__builtin_thread_pointer();

  dprintf(2, "GUEST_SERVICE ready=1 pid=%d tid=%d tp=%llx\n", s->guest_pid,
          s->guest_tid, (unsigned long long)s->guest_tp);
  atomic_store_explicit(&s->phase, 2, memory_order_release);

  while (full_guest_wait(s)) {
    atomic_fetch_add(&s->heartbeat, 1);
  }
  full_guest_shutdown();

  /* exit()/return from main would terminate the Host thread group. */
  syscall(SYS_exit, 0);
  __builtin_unreachable();
}
