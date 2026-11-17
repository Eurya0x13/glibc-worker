#define _GNU_SOURCE
#include "signal-execution.h"
#include "nonlocal-mask.h"
#include "signal-stack.h"
#include <errno.h>
#include <setjmp.h>

extern void domain_start(void);
static _Thread_local struct gw_signal_execution *active;

static _Noreturn void finish(struct gw_signal_execution *execution,
                             enum gw_signal_exit reason) {
  gw_nonlocal_mask_block();
  int saved = errno;
  execution->finish(execution->argument, reason);
  active = execution->previous;
  execution->landing = NULL;
  execution->phase = reason == GW_SIGNAL_RETURN ? GW_SIGNAL_EXECUTION_DONE
                                                : GW_SIGNAL_EXECUTION_ABORTED;
  errno = saved;

  /* host is captured again on the recovery landing, never the abandoned
   * caller's old C frame. No return through that old stack is possible. */
  execution->swap(&execution->guest, &execution->host);
  _exit(125);
}

static _Noreturn void run(void *argument) {
  struct gw_signal_execution *execution = argument;
  sigjmp_buf landing;
  execution->landing = &landing;
  execution->previous = active;
  active = execution;
  execution->phase = GW_SIGNAL_EXECUTION_RUNNING;
  gw_signal_stack_set(&execution->alternate);

  int reason = sigsetjmp(landing, 0);
  if (!reason) {
    execution->result = execution->invoke(execution->argument);
  }
  finish(execution, reason);
}

static _Noreturn void abandon(void *argument) {
  struct gw_signal_execution *execution = argument;
  if (execution != active || execution->phase != GW_SIGNAL_EXECUTION_RUNNING ||
      !execution->landing) {
    _exit(125);
  }
  /* Only this libc interprets its buffer. This landing retires bridge state;
   * longjmp does not unwind application C++ or pthread cleanup handlers.
   * https://github.com/bminor/glibc/blob/glibc-2.44/setjmp/longjmp.c */
  gw_signal_stack_set(&execution->alternate);
  siglongjmp(*(sigjmp_buf *)execution->landing, GW_SIGNAL_HOST_JUMP);
}

int gw_guest_signal_internal_jump(const void *buffer) {
  return active && active->landing == buffer;
}

_Noreturn void gw_guest_signal_escape(void) {
  if (!active || !active->landing) {
    _exit(125);
  }
  siglongjmp(*(sigjmp_buf *)active->landing, GW_SIGNAL_GUEST_JUMP);
}

void gw_guest_signal_entries(struct gw_signal_execution_entries *entries) {
  *entries = (struct gw_signal_execution_entries){(uintptr_t)domain_start, run,
                                                  abandon};
}

int gw_guest_signal_execution_init(
    struct gw_signal_execution *execution,
    const struct gw_signal_execution_config *config) {
  struct gw_signal_execution_entries entries;
  gw_guest_signal_entries(&entries);
  struct domain saved = {.tp = (uintptr_t)__builtin_thread_pointer()};
#if defined(__x86_64__)
  __asm__ volatile("stmxcsr %0" : "=m"(saved.saved[6]));
  __asm__ volatile("fnstcw %0" : "=m"(saved.saved[7]));
#else
  __asm__ volatile("mov %0, x18" : "=r"(saved.saved[12]));
  __asm__ volatile("mrs %0, fpcr" : "=r"(saved.saved[21]));
  __asm__ volatile("mrs %0, fpsr" : "=r"(saved.saved[22]));
#endif
  return gw_signal_execution_from_saved(execution, config, &saved, &entries);
}
