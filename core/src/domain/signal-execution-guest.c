#define _GNU_SOURCE
#include "signal-execution.h"
#include "nonlocal-mask.h"
#include "signal-stack.h"
#include <errno.h>
#include <setjmp.h>
#include <string.h>

extern void domain_start(void);

static _Noreturn void finish(struct gw_signal_execution *execution,
                             int abandoned) {
  gw_nonlocal_mask_block();
  int saved = errno;
  execution->finish(execution->argument, abandoned);
  execution->landing = NULL;
  execution->phase =
      abandoned ? GW_SIGNAL_EXECUTION_ABORTED : GW_SIGNAL_EXECUTION_DONE;
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
  execution->phase = GW_SIGNAL_EXECUTION_RUNNING;
  gw_signal_stack_set(&execution->alternate);

  if (!sigsetjmp(landing, 0)) {
    execution->result = execution->invoke(execution->argument);
    finish(execution, 0);
  }
  finish(execution, 1);
}

static _Noreturn void abandon(void *argument) {
  struct gw_signal_execution *execution = argument;
  if (execution->phase != GW_SIGNAL_EXECUTION_RUNNING || !execution->landing) {
    _exit(125);
  }
  /* Only this libc interprets its buffer. This landing retires bridge state;
   * longjmp does not unwind application C++ or pthread cleanup handlers.
   * https://github.com/bminor/glibc/blob/glibc-2.44/setjmp/longjmp.c */
  gw_signal_stack_set(&execution->alternate);
  siglongjmp(*(sigjmp_buf *)execution->landing, 1);
}

static int valid_stack(void *stack, size_t size) {
  return stack && size >= 16384 && size <= UINTPTR_MAX - (uintptr_t)stack;
}

static void initialize_domain(struct domain *domain, void *stack, size_t size,
                              struct gw_signal_execution *execution,
                              void (*entry)(void *)) {
  domain->tp = (uintptr_t)__builtin_thread_pointer();
  domain->sp = ((uintptr_t)stack + size) & ~(uintptr_t)15;
  domain->pc = (uintptr_t)domain_start;
#if defined(__x86_64__)
  domain->saved[2] = (uintptr_t)execution;
  domain->saved[3] = (uintptr_t)entry;
  __asm__ volatile("stmxcsr %0" : "=m"(domain->saved[6]));
  __asm__ volatile("fnstcw %0" : "=m"(domain->saved[7]));
#else
  domain->saved[0] = (uintptr_t)execution;
  domain->saved[1] = (uintptr_t)entry;
  __asm__ volatile("mov %0, x18" : "=r"(domain->saved[12]));
  __asm__ volatile("mrs %0, fpcr" : "=r"(domain->saved[21]));
  __asm__ volatile("mrs %0, fpsr" : "=r"(domain->saved[22]));
#endif
}

int gw_guest_signal_execution_init(
    struct gw_signal_execution *execution,
    const struct gw_signal_execution_config *config) {
  if (!execution || !config || !config->swap || !config->invoke ||
      !config->finish || !valid_stack(config->stack, config->stack_size) ||
      !valid_stack(config->abort_stack, config->abort_stack_size) ||
      !valid_stack(config->alternate.ss_sp, config->alternate.ss_size) ||
      config->alternate.ss_flags) {
    return EINVAL;
  }
  uintptr_t low = (uintptr_t)config->stack;
  uintptr_t abort_low = (uintptr_t)config->abort_stack;
  uintptr_t alternate_low = (uintptr_t)config->alternate.ss_sp;
  if ((low < abort_low + config->abort_stack_size &&
       abort_low < low + config->stack_size) ||
      (alternate_low < abort_low + config->abort_stack_size &&
       abort_low < alternate_low + config->alternate.ss_size)) {
    return EINVAL;
  }

  memset(execution, 0, sizeof *execution);
  execution->swap = config->swap;
  execution->invoke = config->invoke;
  execution->finish = config->finish;
  execution->argument = config->argument;
  execution->alternate = config->alternate;
  execution->version = 1;
  execution->phase = GW_SIGNAL_EXECUTION_PREPARED;
  initialize_domain(&execution->guest, config->stack, config->stack_size,
                    execution, run);
  initialize_domain(&execution->abort, config->abort_stack,
                    config->abort_stack_size, execution, abandon);
  return 0;
}
