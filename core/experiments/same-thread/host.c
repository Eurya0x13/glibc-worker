#define _GNU_SOURCE
#include "domain.h"
#include "loader_image.h"
#include "raw_syscall.h"
#include <errno.h>
#include <fenv.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

static __thread int host_counter = 17;
static struct exchange exchange;

/* Can run while Guest TLS is active. Only raw instructions and shared POD;
 * never invoke the Host's errno accessor, allocator or signal dispatcher. */
static __attribute__((no_stack_protector)) void observe_signal(int signo) {
  (void)signo;
  uintptr_t tp = 0;
#if defined(__x86_64__)
  gw_raw3(SYS_arch_prctl, 0x1003, (long)&tp, 0);
#else
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
#endif
  sig_atomic_t index = exchange.signal_count;
  if (index < 2) {
    exchange.signal_tp[index] = tp;
    exchange.signal_count = index + 1;
  }
}

static void host_check(struct exchange *e) {
  CHECK(e, errno == EDOM);
  CHECK(e, host_counter == 17 && fegetround() == FE_DOWNWARD);
  CHECK(e, syscall(SYS_gettid) == e->host_tid);
  int local;
  CHECK(e,
        (uintptr_t)&local < e->stack_low || (uintptr_t)&local >= e->stack_high);
}

static int invoke(struct exchange *e) {
  e->state = DOMAIN_CALL;
  e->value = 37;
  e->swap(&e->host, &e->guest);
  host_check(e);
  while (e->state == DOMAIN_CALLBACK) {
    int value = e->value;
    CHECK(e, value == 42);
    /* Read Guest-owned storage while Host TLS is active; only Guest frees it.
     */
    CHECK(e, e->allocation && *e->allocation == value);
    ++e->callbacks;
    if (e->depth + 1 < DOMAIN_NESTING) {
      ++e->depth;
      CHECK(e, invoke(e) == 84);
      --e->depth;
      ++e->nested;
    }
    e->value = value * 2;
    e->state = DOMAIN_DONE;
    e->swap(&e->host, &e->guest);
    host_check(e);
  }
  CHECK(e, e->state == DOMAIN_DONE && e->value == 84);
  return e->value;
}

static void *guarded_stack(size_t size) {
  size_t page = (size_t)sysconf(_SC_PAGESIZE);
  char *mapping = mmap(NULL, size + 2 * page, PROT_NONE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mapping == MAP_FAILED ||
      mprotect(mapping + page, size, PROT_READ | PROT_WRITE)) {
    perror("Guest stack");
    exit(1);
  }
  return mapping + page;
}

static void prepare(struct exchange *e, char **argv) {
  struct sigaction action = {.sa_handler = observe_signal};
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGUSR1, &action, NULL)) {
    perror("install diagnostic handler");
    exit(1);
  }
  e->swap = domain_swap;
  e->dso = argv[4];
  e->host_tid = syscall(SYS_gettid);
  e->host_tls = (uintptr_t)&host_counter;
  const size_t stack_size = 2 * 1024 * 1024;
  void *stack = guarded_stack(stack_size);
  e->stack_low = (uintptr_t)stack;
  e->stack_high = e->stack_low + stack_size;
  char pointer[32];
  snprintf(pointer, sizeof pointer, "%lx", (unsigned long)(uintptr_t)e);
  char *guest_argv[] = {argv[1], "--library-path", argv[2], argv[3], pointer};
  struct gw_bootstrap_image loader = gw_bootstrap_map(argv[1]);
  e->guest.sp =
      (uintptr_t)gw_bootstrap_stack(loader, stack, stack_size, guest_argv,
                                    sizeof guest_argv / sizeof guest_argv[0]);
  e->guest.pc = (uintptr_t)loader.entry;
  CHECK(e, fesetround(FE_DOWNWARD) == 0);
#if defined(__x86_64__)
  __asm__ volatile("stmxcsr %0" : "=m"(e->guest.saved[6]));
  __asm__ volatile("fnstcw %0" : "=m"(e->guest.saved[7]));
#endif
}

struct kernel_state {
  void *clear_tid, *robust;
  int clear_error, robust_error;
};

/* Observe loader takeover without guessing private pthread offsets. Some
 * kernels/emulators do not expose these queries; retain their error codes. */
static struct kernel_state snapshot(void) {
  struct kernel_state state = {0};
  int saved_errno = errno;
  if (prctl(PR_GET_TID_ADDRESS, &state.clear_tid, 0, 0, 0)) {
    state.clear_error = errno;
  }
  size_t robust_length = 0;
  if (syscall(SYS_get_robust_list, 0, &state.robust, &robust_length)) {
    state.robust_error = errno;
  }
  errno = saved_errno;
  return state;
}

static void check_startup(struct exchange *e) {
  host_check(e);
  CHECK(e, e->state == DOMAIN_READY && e->host_tid == e->guest_tid);
  CHECK(e, e->host.tp && e->guest.tp && e->host.tp != e->guest.tp);
  CHECK(e, e->host_tls != e->guest_tls);
  CHECK(e, raise(SIGUSR1) == 0);
  CHECK(e, e->signal_count == 2 && e->signal_tp[0] == e->guest.tp &&
               e->signal_tp[1] == e->host.tp);
}

static void exercise(struct exchange *e) {
  for (int i = 0; i < DOMAIN_ROUNDS; ++i) {
    CHECK(e, invoke(e) == 84);
  }
  CHECK(e, e->calls == DOMAIN_ROUNDS * DOMAIN_NESTING);
  CHECK(e, e->callbacks == e->calls);
  CHECK(e, e->entries == e->calls && e->allocation == NULL);
  CHECK(e, e->nested == DOMAIN_ROUNDS * (DOMAIN_NESTING - 1));
  CHECK(e,
        e->dso_tls && e->dso_tls != e->host_tls && e->dso_tls != e->guest_tls);
}

static void finish(struct exchange *e, struct kernel_state before,
                   struct kernel_state after) {
  int clear_observed = !before.clear_error && !after.clear_error;
  int robust_observed = !before.robust_error && !after.robust_error;
  /* Restore only the kernel registration whose original pointer we know.
   * This does not make Guest pthread_exit/join/cancel safe. */
  if (clear_observed) {
    CHECK(e, syscall(SYS_set_tid_address, before.clear_tid) == e->host_tid);
  }
  printf(
      "{\"result\":\"%s\",\"failures\":%d,\"calls\":%d,"
      "\"callbacks\":%d,\"nested\":%d,\"same_tid\":%s,"
      "\"distinct_tls\":%s,\"clear_tid_observed\":%s,"
      "\"clear_tid_changed\":%s,\"robust_observed\":%s,"
      "\"clear_tid_errno_before\":%d,\"clear_tid_errno_after\":%d,"
      "\"robust_errno_before\":%d,\"robust_errno_after\":%d,"
      "\"robust_changed\":%s,\"signals_validated\":false,"
      "\"host_signal_saw_guest_tls\":%s,"
      "\"thread_lifecycle_validated\":false}\n",
      e->failures ? "FAIL" : "PASS", e->failures, e->calls, e->callbacks,
      e->nested, e->host_tid == e->guest_tid ? "true" : "false",
      e->host.tp != e->guest.tp ? "true" : "false",
      clear_observed ? "true" : "false",
      !clear_observed                       ? "null"
      : before.clear_tid != after.clear_tid ? "true"
                                            : "false",
      robust_observed ? "true" : "false", before.clear_error, after.clear_error,
      before.robust_error, after.robust_error,
      !robust_observed                ? "null"
      : before.robust != after.robust ? "true"
                                      : "false",
      e->signal_tp[0] == e->guest.tp ? "true" : "false");
  fflush(stdout);
  /* No shared-runtime teardown fiction: the supervisor discards this whole
   * process, including both libc heaps, stacks, TLS and mapped images. */
  _exit(e->failures ? 1 : 0);
}

int main(int argc, char **argv) {
  if (argc != 5) {
    fprintf(stderr, "usage: host LOADER RUNTIME GUEST DSO\n");
    return 2;
  }
#if defined(__x86_64__)
  unsigned long shadow_stack = 0;
  if (syscall(SYS_arch_prctl, 0x5005, &shadow_stack) == 0 && shadow_stack) {
    fprintf(stderr, "Active CET shadow stack is not supported by this spike\n");
    return 77;
  }
#endif
  struct exchange *e = &exchange;
  prepare(e, argv);
  struct kernel_state before = snapshot();
  errno = EDOM;
  e->swap(&e->host, &e->guest);
  check_startup(e);
  struct kernel_state after = snapshot();
  errno = EDOM;
  exercise(e);
  finish(e, before, after);
}
