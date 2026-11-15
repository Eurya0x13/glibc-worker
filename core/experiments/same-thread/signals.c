#define _GNU_SOURCE
#include "signals.h"
#include "raw_syscall.h"
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/mman.h>
#include <ucontext.h>

struct signal_slot signal_slots[ADOPT_THREADS];
static int signal_try_register(struct signal_slot *, struct exchange *);
#include "signal-registry.inc"
extern void domain_signal_entry(int, siginfo_t *, void *);
#ifdef GW_DOMAIN_CHAIN
#include "signal-router.h"
_Atomic int domain_forwarded_faults;
_Atomic int domain_forwarded_async;
_Atomic int domain_guest_host_signals;
_Atomic int domain_expected_host_signals;
_Atomic int domain_sr_deliveries;
_Atomic int domain_sr_guest_deliveries;
static _Atomic long guest_tids[ADOPT_THREADS + 1];
static _Atomic uintptr_t guest_tps[ADOPT_THREADS + 1];

/* Callable with Guest TLS: no libc, allocation, locks or stack protector. */
__attribute__((no_stack_protector)) void signal_note_guest_thread(
    int index, long tid, uintptr_t tp) {
  atomic_store_explicit(&guest_tps[index], tp, memory_order_relaxed);
  atomic_store_explicit(&guest_tids[index], tid, memory_order_release);
}

static void forward_host(int number, siginfo_t *info, void *context,
                         void *token) {
  if (number == SIGUSR2) {
    atomic_fetch_add_explicit(&domain_sr_deliveries, 1, memory_order_relaxed);
  }
  if (number == SIGSEGV) {
    atomic_fetch_add_explicit(&domain_forwarded_faults, 1,
                              memory_order_relaxed);
  } else {
    atomic_fetch_add_explicit(&domain_forwarded_async, 1, memory_order_relaxed);
  }
  signal_router_forward(number, info, context, token);
}
#endif

/* Executed before the Host TP is restored. Must remain TLS/libc-free. */
__attribute__((no_stack_protector)) struct signal_slot *domain_signal_lookup(
    uintptr_t interrupted_tp) {
  long tid = gw_raw3(SYS_gettid, 0, 0, 0);
  struct signal_slot *dynamic = signal_dynamic_lookup(tid, interrupted_tp);
  if (dynamic) {
    return dynamic;
  }
  for (int i = 0; i < ADOPT_THREADS; ++i) {
    if (atomic_load_explicit(&signal_slots[i].tid, memory_order_acquire) ==
        tid) {
      return &signal_slots[i];
    }
  }
#ifdef GW_DOMAIN_CHAIN
  for (int i = 0; i <= ADOPT_THREADS; ++i) {
    if (atomic_load_explicit(&guest_tids[i], memory_order_acquire) == tid &&
        atomic_load_explicit(&guest_tps[i], memory_order_relaxed) ==
            interrupted_tp) {
      return (struct signal_slot *)(uintptr_t)-1;
    }
  }
#else
  (void)interrupted_tp;
#endif
  return NULL;
}

/* Host TLS and (on ARM64) x18 are valid here. Original kernel ucontext is
 * forwarded unchanged, except for the fixture's explicitly recognized fault. */
void domain_signal_dispatch(int number, siginfo_t *info, void *context,
                            struct signal_slot *slot, uintptr_t interrupted_tp,
                            void *token) {
#ifndef GW_DOMAIN_CHAIN
  (void)token;
  (void)interrupted_tp;
#endif
#ifdef GW_DOMAIN_CHAIN
  if (slot && number == SIGUSR2 && interrupted_tp != slot->host_tp) {
    atomic_fetch_add_explicit(&domain_sr_guest_deliveries, 1,
                              memory_order_relaxed);
  }
  if (!slot) {
    forward_host(number, info, context, token);
    return;
  }
#endif
  int saved_errno = errno;
  atomic_fetch_add_explicit(&slot->failures, !adoption_signal_check(slot),
                            memory_order_relaxed);
  if (number == SIGSEGV) {
    ucontext_t *uc = context;
#if defined(__x86_64__)
    uintptr_t pc = uc->uc_mcontext.gregs[REG_RIP];
#else
    uintptr_t pc = uc->uc_mcontext.pc;
#endif
    if (pc != (uintptr_t)domain_fault_instruction ||
        info->si_addr != slot->exchange->fault_page) {
#ifdef GW_DOMAIN_CHAIN
      /* An unrelated Guest fault is not a JVM implicit exception. Fail
       * closed instead of asking HotSpot to interpret a foreign stack. */
      if (interrupted_tp != slot->host_tp) {
        _exit(96);
      }
      forward_host(number, info, context, token);
      errno = saved_errno;
      return;
#else
      gw_raw3(SYS_exit_group, 96, 0, 0);
      __builtin_unreachable();
#endif
    }
#if defined(__x86_64__)
    uc->uc_mcontext.gregs[REG_RIP] = (greg_t)domain_fault_resume;
#else
    uc->uc_mcontext.pc = (uintptr_t)domain_fault_resume;
#endif
    atomic_fetch_add_explicit(&slot->faults, 1, memory_order_relaxed);
  } else if (number == DOMAIN_NOTIFY_SIGNAL || number == DOMAIN_NESTED_SIGNAL) {
    atomic_fetch_add_explicit(&slot->signals, 1, memory_order_relaxed);
    if (number == DOMAIN_NOTIFY_SIGNAL) {
      /* DOMAIN_NESTED_SIGNAL is unmasked while this handler runs: nested Host
       * dispatch. */
      gw_raw3(SYS_tgkill, gw_raw3(SYS_getpid, 0, 0, 0),
              gw_raw3(SYS_gettid, 0, 0, 0), DOMAIN_NESTED_SIGNAL);
    }
  }
#ifdef GW_DOMAIN_CHAIN
  else {
    if (interrupted_tp != slot->host_tp && info->si_code > 0 &&
        (number == SIGBUS || number == SIGILL || number == SIGFPE)) {
      _exit(96);
    }
    if (interrupted_tp != slot->host_tp) {
      int index = number == SIGUSR2   ? 0
                  : number == SIGPIPE ? 1
                  : number == SIGXFSZ ? 2
                                      : -1;
      if (index >= 0) {
        ++slot->exchange->host_signal_delivered[index];
      }
      atomic_fetch_add_explicit(&domain_guest_host_signals, 1,
                                memory_order_relaxed);
    }
    forward_host(number, info, context, token);
  }
#endif
  errno = saved_errno;
}

void signal_install(void) {
  struct sigaction existing;
  const int owned[] = {DOMAIN_NOTIFY_SIGNAL, DOMAIN_NESTED_SIGNAL};
  for (unsigned i = 0; i < sizeof owned / sizeof owned[0]; ++i) {
    if (sigaction(owned[i], NULL, &existing) ||
        existing.sa_handler != SIG_DFL) {
      fprintf(stderr,
              "SIGNAL_SETUP occupied=%d handler=%p flags=0x%x errno=%d\n",
              owned[i], (void *)existing.sa_handler, existing.sa_flags, errno);
      _exit(97);
    }
  }
  struct sigaction action = {.sa_sigaction = domain_signal_entry,
                             .sa_flags = SA_SIGINFO | SA_ONSTACK};
  sigemptyset(&action.sa_mask);
  if (sigaction(DOMAIN_NOTIFY_SIGNAL, &action, NULL) ||
      sigaction(DOMAIN_NESTED_SIGNAL, &action, NULL)) {
    _exit(97);
  }
#ifndef GW_DOMAIN_CHAIN
  if (sigaction(SIGSEGV, &action, NULL)) {
    _exit(97);
  }
#endif
}

static int signal_try_register(struct signal_slot *slot, struct exchange *e) {
#if defined(__x86_64__)
  if (gw_raw3(SYS_arch_prctl, 0x1003, (long)&slot->host_tp, 0)) {
    return -1;
  }
#else
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(slot->host_tp));
  __asm__ volatile("mov %0, x18" : "=r"(slot->host_x18));
#endif
  slot->exchange = e;
  slot->signal_stack = mmap(NULL, 65536, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (slot->signal_stack == MAP_FAILED) {
    return -1;
  }
  stack_t alternate = {.ss_sp = slot->signal_stack, .ss_size = 65536};
  if (sigaltstack(&alternate, &slot->old_stack)) {
    munmap(slot->signal_stack, 65536);
    return -1;
  }
  atomic_store_explicit(&slot->tid, e->host_tid, memory_order_release);
  sigset_t synthetic;
  sigemptyset(&synthetic);
  sigaddset(&synthetic, DOMAIN_NOTIFY_SIGNAL);
  sigaddset(&synthetic, DOMAIN_NESTED_SIGNAL);
  if (e->host_signal_probe) {
    /* The application exec path can inherit SIGPIPE blocked. Explicitly
     * enable delivery for this fixture, after registration, and restore the
     * entire inherited mask on exit. This is not Guest mask virtualization. */
    sigaddset(&synthetic, SIGUSR2);
    sigaddset(&synthetic, SIGPIPE);
    sigaddset(&synthetic, SIGXFSZ);
  }
  if (sigprocmask(SIG_UNBLOCK, &synthetic, &slot->inherited_mask)) {
    atomic_store_explicit(&slot->tid, 0, memory_order_release);
    sigaltstack(&slot->old_stack, NULL);
    munmap(slot->signal_stack, 65536);
    return -1;
  }
  slot->inherited_blocked =
      sigismember(&slot->inherited_mask, DOMAIN_NOTIFY_SIGNAL) ||
      sigismember(&slot->inherited_mask, DOMAIN_NESTED_SIGNAL);
  return 0;
}

void signal_register(struct signal_slot *slot, struct exchange *e) {
  if (signal_try_register(slot, e)) {
    _exit(98);
  }
}

void signal_unregister(struct signal_slot *slot) {
  /* Senders have quiesced. Block/drain only our synthetic asynchronous
   * signals before revoking the registry entry. Never mask hardware faults. */
  sigset_t synthetic;
  sigemptyset(&synthetic);
  sigaddset(&synthetic, DOMAIN_NOTIFY_SIGNAL);
  sigaddset(&synthetic, DOMAIN_NESTED_SIGNAL);
  if (sigprocmask(SIG_BLOCK, &synthetic, NULL)) {
    _exit(98);
  }
  struct timespec zero = {0};
  while (sigtimedwait(&synthetic, NULL, &zero) >= 0) {
  }
  atomic_store_explicit(&slot->tid, 0, memory_order_release);
  if (sigaltstack(&slot->old_stack, NULL)) {
    _exit(98);
  }
  munmap(slot->signal_stack, 65536);
  if (sigprocmask(SIG_SETMASK, &slot->inherited_mask, NULL)) {
    _exit(98);
  }
  sigset_t restored;
  if (sigprocmask(SIG_SETMASK, NULL, &restored)) {
    _exit(98);
  }
  /* Compare membership rather than padding in libc-specific sigset_t. */
  for (int number = 1; number < NSIG; ++number) {
    atomic_fetch_add_explicit(&slot->failures,
                              sigismember(&restored, number) !=
                                  sigismember(&slot->inherited_mask, number),
                              memory_order_relaxed);
  }
}

void signal_send(void) {
  long pid = syscall(SYS_getpid);
  for (int i = 0; i < ADOPT_THREADS; ++i) {
    long tid = atomic_load_explicit(&signal_slots[i].tid, memory_order_acquire);
    if (tid) {
      syscall(SYS_tgkill, pid, tid, DOMAIN_NOTIFY_SIGNAL);
    }
  }
}
