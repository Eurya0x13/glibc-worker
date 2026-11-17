#ifndef GW_DOMAIN_SIGNAL_STACK_H
#define GW_DOMAIN_SIGNAL_STACK_H
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#define GW_SS_AUTODISARM (UINT32_C(1) << 31)

void domain_stack_call(void (*)(void *), void *, uintptr_t);
/* Record the lowest suspended caller SP at the actual stack switch. Nested
 * delivery must not reuse space occupied by these live transition frames. */
void domain_stack_call_record(void (*)(void *), void *, uintptr_t, uintptr_t *);

static inline int gw_stack_contains(const stack_t *stack, uintptr_t sp) {
  return !(stack->ss_flags & SS_DISABLE) &&
         sp - (uintptr_t)stack->ss_sp < stack->ss_size;
}

static inline uintptr_t gw_signal_interrupted_sp(const ucontext_t *context) {
#if defined(__x86_64__)
  return context->uc_mcontext.gregs[REG_RSP];
#else
  return context->uc_mcontext.sp;
#endif
}

static inline uintptr_t gw_signal_stack_top(uintptr_t sp) {
#if defined(__x86_64__)
  /* The interrupted function owns the 128-byte SysV red zone.
   * https://github.com/torvalds/linux/blob/v6.12/arch/x86/kernel/signal.c */
  sp -= 128;
#endif
  return sp & ~(uintptr_t)15;
}

/* Switch only after SP has left the previously configured alternate stack.
 * Preserve errno, strip the query-only SS_ONSTACK flag and keep AUTODISARM.
 * https://github.com/torvalds/linux/blob/v6.12/kernel/signal.c */
static inline void gw_signal_stack_get(stack_t *stack) {
  int saved = errno;
  if (syscall(SYS_sigaltstack, NULL, stack)) {
    _exit(125);
  }
  stack->ss_flags &= ~SS_ONSTACK;
  errno = saved;
}

static inline void gw_signal_stack_set(const stack_t *stack) {
  int saved = errno;
  stack_t value = *stack;
  value.ss_flags &= ~SS_ONSTACK;
  if (syscall(SYS_sigaltstack, &value, NULL)) {
    _exit(125);
  }
  errno = saved;
}
#endif
