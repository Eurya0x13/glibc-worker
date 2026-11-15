#define _GNU_SOURCE
#include "signal-router.h"
#include <dlfcn.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

extern void domain_signal_entry(int, siginfo_t *, void *);

struct disposition {
  struct sigaction action;
  _Atomic int fired;
  void *entry;
};

/* Records are immutable after publication. A signal may interrupt an updater
 * on any thread; no reader takes the writer lock or touches malloc. Retain
 * records until the disposable process exits, including across siglongjmp.
 * Production reclamation is intentionally not claimed by this experiment. */
static _Atomic(struct disposition *) current[NSIG];
static struct sigaction original[NSIG];
static unsigned char managed[NSIG];
static _Atomic unsigned records;
static atomic_flag writer = ATOMIC_FLAG_INIT;
static int (*system_action)(int, const struct sigaction *, struct sigaction *);
static _Atomic int enabled;
static struct disposition default_action;

static void lock_writer(uint64_t *previous) {
  uint64_t all = UINT64_MAX;
  /* Block all catchable signals before taking the lock. A handler cannot
   * interrupt this thread while it owns the lock and recursively deadlock. */
  if (syscall(SYS_rt_sigprocmask, SIG_SETMASK, &all, previous, 8)) {
    _exit(105);
  }
  while (atomic_flag_test_and_set_explicit(&writer, memory_order_acquire)) {
  }
}

static void unlock_writer(uint64_t previous) {
  atomic_flag_clear_explicit(&writer, memory_order_release);
  if (syscall(SYS_rt_sigprocmask, SIG_SETMASK, &previous, NULL, 8)) {
    _exit(105);
  }
}

int signal_router_init_with_backend(signal_router_backend backend) {
  if (atomic_load(&enabled)) {
    return EALREADY;
  }
  system_action = backend;
  if (!system_action) {
    return ENOSYS;
  }
  /* The fixture covers HotSpot's default SR signal only. Do not silently
   * allow an overridden signal to bypass TLS restoration or Guest ownership. */
  if (getenv("_JAVA_SR_SIGNUM")) {
    return ENOTSUP;
  }
  const int signals[] = {SIGSEGV, SIGBUS,  SIGILL, SIGFPE, SIGPIPE,
                         SIGXFSZ, SIGUSR2, SIGHUP, SIGINT, SIGTERM,
                         SIGQUIT, 50,      51,     52,     53};
  for (unsigned i = 0; i < sizeof signals / sizeof signals[0]; ++i) {
    int number = signals[i];
    if (system_action(number, NULL, &original[number])) {
      return errno;
    }
    atomic_store(&current[number], NULL);
    managed[number] = 1;
  }
  atomic_store_explicit(&enabled, 1, memory_order_release);
  return 0;
}

int signal_router_init(void) {
  return signal_router_init_with_backend(dlsym(RTLD_NEXT, "sigaction"));
}

static int same_action(const struct sigaction *left,
                       const struct sigaction *right) {
  if (left->sa_handler != right->sa_handler ||
      left->sa_flags != right->sa_flags) {
    return 0;
  }
  for (int number = 1; number < NSIG; ++number) {
    if (sigismember(&left->sa_mask, number) !=
        sigismember(&right->sa_mask, number)) {
      return 0;
    }
  }
  return 1;
}

/* Only for an explicit late-loading integration, before enabling any Guest
 * carrier. The integrator must serialize Host registration during adoption.
 * Deliveries remain safe: immutable entries are complete before installation.
 * A layered backend cannot establish that its outer handler restores TLS. */
int signal_router_adopt_chain(void *reviewed_chain_base) {
  if (!atomic_load_explicit(&enabled, memory_order_acquire)) {
    return EINVAL;
  }
  for (int number = 1; number < NSIG; ++number) {
    if (!managed[number]) {
      continue;
    }
    struct sigaction actual;
    if (system_action(number, NULL, &actual)) {
      return errno;
    }
    if (!same_action(&actual, &original[number]) ||
        atomic_load(&current[number])) {
      return EBUSY;
    }
    Dl_info owner = {0};
    if (actual.sa_handler != SIG_DFL && actual.sa_handler != SIG_IGN &&
        dladdr((void *)actual.sa_sigaction, &owner) && owner.dli_fname &&
        (strstr(owner.dli_fname, "libsigchain.so") ||
         strstr(owner.dli_fname, "libart.so")) &&
        owner.dli_fbase != reviewed_chain_base) {
      /* ART reads pthread TLS before its special handlers. Wrapping only the
       * user handler is too late; do not mutate this chain speculatively. */
      return ENOTSUP;
    }
  }
  for (int number = 1; number < NSIG; ++number) {
    if (managed[number] && original[number].sa_handler != SIG_DFL &&
        original[number].sa_handler != SIG_IGN &&
        signal_router_action(number, &original[number], NULL)) {
      /* The transaction owner restores all layers exactly once. Partial
       * adoption retains immutable entries until process disposal. */
      return errno;
    }
  }
  return 0;
}

int signal_router_adopt(void) {
  return signal_router_adopt_chain(NULL);
}

/* A distinct immutable entry binds the kernel action (including its mask
 * and flags) to the corresponding logical handler. Looking up current[] at
 * delivery time would mix an old kernel frame with a newer registration. */
static int make_entry(struct disposition *record) {
  size_t page = (size_t)sysconf(_SC_PAGESIZE);
  unsigned char *code = mmap(NULL, page, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (code == MAP_FAILED) {
    return -1;
  }
  uintptr_t token = (uintptr_t)record, target = (uintptr_t)domain_signal_entry;
#if defined(__aarch64__)
  const uint32_t instructions[] = {0xd503245f, 0x58000063, 0x58000090,
                                   0xd61f0200};
  memcpy(code, instructions, sizeof instructions);
  memcpy(code + 16, &token, 8);
  memcpy(code + 24, &target, 8);
#else
  const unsigned char instructions[] = {
      0xf3, 0x0f, 0x1e, 0xfa, 0x48, 0xb9, 0, 0, 0, 0, 0,    0,    0,   0,
      0x49, 0xbb, 0,    0,    0,    0,    0, 0, 0, 0, 0x41, 0xff, 0xe3};
  memcpy(code, instructions, sizeof instructions);
  memcpy(code + 6, &token, 8);
  memcpy(code + 16, &target, 8);
#endif
  __builtin___clear_cache((char *)code, (char *)code + 32);
  if (mprotect(code, page, PROT_READ | PROT_EXEC)) {
    int error = errno;
    munmap(code, page);
    errno = error;
    return -1;
  }
  record->entry = code;
  return 0;
}

/* Exported by the experiment executable so libjvm's public sigaction calls
 * enter here. Queries return the logical disposition, not the routing entry.
 * Explicit dlsym(libc, ...) and raw syscalls are outside this interception. */
int signal_router_action(int number, const struct sigaction *action,
                         struct sigaction *previous) {
  if (!system_action) {
    errno = ENOSYS;
    return -1;
  }
  if (!atomic_load_explicit(&enabled, memory_order_acquire) || number <= 0 ||
      number >= NSIG || !managed[number]) {
    return system_action(number, action, previous);
  }
  uint64_t mask;
  lock_writer(&mask);
  struct disposition *old =
      atomic_load_explicit(&current[number], memory_order_acquire);
  if (!action) {
    struct sigaction actual;
    int status = system_action(number, NULL, &actual);
    int error = errno;
    if (!status && previous) {
      *previous = old ? old->action : original[number];
      /* Preserve kernel feature probing (notably ART's SA_UNSUPPORTED).
       * Hide our synthetic SA_SIGINFO while removing request bits that the
       * kernel did not accept. Immutable records remain safe for delivery. */
      previous->sa_flags &= actual.sa_flags;
      /* SA_RESETHAND resets in the kernel before our entry can run. The
       * carrier may be descheduled in that window; fired is not authoritative.
       */
      previous->sa_handler =
          actual.sa_handler == SIG_DFL || actual.sa_handler == SIG_IGN
              ? actual.sa_handler
              : previous->sa_handler;
    }
    unlock_writer(mask);
    errno = error;
    return status;
  }
  /* A page per update avoids an allocator lock in a signal handler. These
   * bounded fixture updates are accounted for; do not use as production GC. */
  struct disposition *next = mmap(NULL, sizeof *next, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (next == MAP_FAILED) {
    int error = errno;
    unlock_writer(mask);
    errno = error;
    return -1;
  }
  next->action = *action;
  struct sigaction installed = *action;
  if (action->sa_handler != SIG_DFL && action->sa_handler != SIG_IGN) {
    if (make_entry(next)) {
      int error = errno;
      munmap(next, sizeof *next);
      unlock_writer(mask);
      errno = error;
      return -1;
    }
    installed.sa_sigaction = next->entry;
    installed.sa_flags |= SA_SIGINFO;
  }
  struct sigaction replaced;
  int status = system_action(number, &installed, &replaced);
  int error = errno;
  if (!status) {
    atomic_store_explicit(&current[number], next, memory_order_release);
    /* The new one-shot may already have fired on another carrier. */
    if (atomic_load_explicit(&next->fired, memory_order_acquire)) {
      struct disposition *expected = next;
      atomic_compare_exchange_strong(&current[number], &expected,
                                     &default_action);
    }
    if (previous) {
      *previous = old ? old->action : original[number];
      previous->sa_flags &= replaced.sa_flags;
      if (replaced.sa_handler == SIG_DFL || replaced.sa_handler == SIG_IGN) {
        previous->sa_handler = replaced.sa_handler;
      }
    }
  }
  atomic_fetch_add_explicit(&records, 1, memory_order_relaxed);
  unlock_writer(mask);
  errno = error;
  return status;
}

int sigaction(int number, const struct sigaction *action,
              struct sigaction *previous) {
  return signal_router_action(number, action, previous);
}

__sighandler_t signal(int number, __sighandler_t handler) {
  struct sigaction action = {.sa_handler = handler, .sa_flags = SA_RESTART};
  struct sigaction previous;
  sigemptyset(&action.sa_mask);
  sigaddset(&action.sa_mask, number);
  return signal_router_action(number, &action, &previous) ? SIG_ERR
                                                          : previous.sa_handler;
}

#ifdef __ANDROID__
/* This experiment builds only LP64 Android. Convert by copying rather than
 * aliasing two distinct struct types through incompatible pointers. */
int signal_router_action64(int number, const struct sigaction64 *action,
                           struct sigaction64 *previous) {
  _Static_assert(sizeof(struct sigaction64) == sizeof(struct sigaction),
                 "LP64 signal ABI required");
  struct sigaction input, output;
  if (action) {
    memcpy(&input, action, sizeof input);
  }
  int result = signal_router_action(number, action ? &input : NULL,
                                    previous ? &output : NULL);
  if (!result && previous) {
    memcpy(previous, &output, sizeof output);
  }
  return result;
}

int sigaction64(int number, const struct sigaction64 *action,
                struct sigaction64 *previous) {
  return signal_router_action64(number, action, previous);
}
#endif

void signal_router_forward(int number, siginfo_t *info, void *context,
                           void *token) {
  int saved_errno = errno;
  struct disposition *record = token;
  if (!record || record->action.sa_handler == SIG_DFL ||
      record->action.sa_handler == SIG_IGN) {
    /* A concurrently replaced disposition cannot be guessed safely. */
    _exit(106);
  }
  struct sigaction action = record->action;
  if (action.sa_flags & SA_RESETHAND) {
    atomic_store_explicit(&record->fired, 1, memory_order_release);
    /* Reset only this generation, never a concurrently installed action. */
    atomic_compare_exchange_strong_explicit(
        &current[number], &record, &default_action, memory_order_acq_rel,
        memory_order_acquire);
  }
  if (action.sa_flags & SA_SIGINFO) {
    action.sa_sigaction(number, info, context);
  } else {
    action.sa_handler(number);
  }
  errno = saved_errno;
}

int signal_router_verify(void) {
  for (int number = 1; number < NSIG; ++number) {
    struct disposition *record =
        atomic_load_explicit(&current[number], memory_order_acquire);
    if (!managed[number]) {
      continue;
    }
    struct sigaction actual;
    if (system_action(number, NULL, &actual)) {
      return errno;
    }
    if (!record) {
      if (!same_action(&actual, &original[number])) {
        return EBUSY;
      }
      continue;
    }
    int custom = record->action.sa_handler != SIG_DFL &&
                 record->action.sa_handler != SIG_IGN;
    if (custom ? (void *)actual.sa_sigaction != record->entry
               : actual.sa_handler != record->action.sa_handler) {
      return EBUSY;
    }
    if (custom) {
      const unsigned flags = SA_RESTART | SA_ONSTACK | SA_NODEFER |
                             SA_RESETHAND | SA_SIGINFO | SA_NOCLDSTOP |
                             SA_NOCLDWAIT;
      if (((unsigned)actual.sa_flags & flags) !=
          (((unsigned)record->action.sa_flags | SA_SIGINFO) & flags)) {
        return EBUSY;
      }
      for (int blocked = 1; blocked < NSIG; ++blocked) {
        if (blocked != SIGKILL && blocked != SIGSTOP &&
            sigismember(&actual.sa_mask, blocked) !=
                sigismember(&record->action.sa_mask, blocked)) {
          return EBUSY;
        }
      }
    }
  }
  return 0;
}

int signal_router_restore(void) {
  /* Caller has joined all test carriers and destroyed the VM. */
  int error = signal_router_verify();
  if (error) {
    return error;  // Do not overwrite a handler installed outside this router.
  }
  for (int number = 1; number < NSIG; ++number) {
    if (managed[number] && system_action(number, &original[number], NULL)) {
      return errno;
    }
  }
  atomic_store(&enabled, 0);
  return 0;
}

unsigned signal_router_registrations(void) {
  return atomic_load(&records);
}

uint64_t signal_router_reserved(void) {
  uint64_t mask = 0;
  for (int number = 1; number < NSIG && number <= 64; ++number) {
    if (managed[number]) {
      mask |= UINT64_C(1) << (number - 1);
    }
  }
  return mask;
}
