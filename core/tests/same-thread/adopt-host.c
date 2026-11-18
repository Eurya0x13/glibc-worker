#define _GNU_SOURCE
#include "adopt.h"
#include "loader_image.h"
#include <errno.h>
#include <fenv.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#ifdef GW_DOMAIN_HOOKS
#include "host-hooks.h"
#endif
#ifdef GW_DOMAIN_SIGNALS
#include "signals.h"
#endif
#ifdef GW_DOMAIN_CHAIN
#include "signal-router.h"
#endif

extern long launch_guest(unsigned long, void *, void *);
static struct adoption control;
static pthread_mutex_t owner_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_barrier_t allocated;
static pthread_key_t host_key;
static _Atomic int host_destructors;
static __thread int host_tls = 17;
#ifdef GW_DOMAIN_SIGNALS
static pthread_barrier_t signals_finished;
static _Atomic int finished_carriers;
static void *fault_page;

int adoption_signal_check(struct signal_slot *slot) {
  return host_tls == 17 && (uintptr_t)&host_tls == slot->exchange->host_tls &&
         syscall(SYS_gettid) == slot->exchange->host_tid;
}
#endif

static void host_destruct(void *value) {
  (void)value;
  atomic_fetch_add(&host_destructors, 1);
}

static void *stack_alloc(size_t size) {
  size_t page = (size_t)sysconf(_SC_PAGESIZE);
  char *memory = mmap(NULL, size + 2 * page, PROT_NONE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (memory == MAP_FAILED ||
      mprotect(memory + page, size, PROT_READ | PROT_WRITE)) {
    perror("stack allocation");
    exit(1);
  }
  return memory + page;
}

static int owner_command(struct carrier *c, int operation) {
  pthread_mutex_lock(&owner_lock);
  int sequence =
      atomic_load_explicit(&control.request, memory_order_relaxed) + 1;
  control.carrier = c;
  control.operation = operation;
  atomic_store_explicit(&control.request, sequence, memory_order_release);
  adopt_wake(&control.request);
  int response;
  while ((response = atomic_load_explicit(&control.response,
                                          memory_order_acquire)) != sequence) {
    adopt_wait(&control.response, response);
  }
  int error = control.error;
  pthread_mutex_unlock(&owner_lock);
  return error;
}

static void host_check(struct exchange *e) {
  CHECK(e, errno == EDOM && host_tls == 17);
  CHECK(e, fegetround() == FE_DOWNWARD);
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
    CHECK(e, value == 42 && e->allocation && *e->allocation == value);
    ++e->callbacks;
#ifdef GW_DOMAIN_HOOKS
    e->value = experiment_callback(e, value, invoke);
#else
    if (e->depth + 1 < DOMAIN_NESTING) {
      ++e->depth;
      CHECK(e, invoke(e) == 84);
      --e->depth;
      ++e->nested;
    }
    e->value = value * 2;
#endif
    e->state = DOMAIN_DONE;
    e->swap(&e->host, &e->guest);
    host_check(e);
  }
  CHECK(e, e->state == DOMAIN_DONE && e->value == 84);
  return e->value;
}

static void prepare_carrier(struct carrier *c) {
  struct exchange *e = &c->exchange;
  e->swap = domain_swap;
  e->host_tid = syscall(SYS_gettid);
  e->host_tls = (uintptr_t)&host_tls;
#ifdef GW_DOMAIN_CHAIN
  e->host_signal_probe = 1;
  extern _Atomic int domain_expected_host_signals;
  const int probes[] = {SIGUSR2, SIGPIPE, SIGXFSZ};
  sigset_t mask;
  CHECK(e, pthread_sigmask(SIG_SETMASK, NULL, &mask) == 0);
  for (int i = 0; i < 3; ++i) {
    struct sigaction action;
    if (sigaction(probes[i], NULL, &action) || action.sa_handler == SIG_DFL) {
      _exit(111); /* Raising a default action is not a forwarding test. */
    }
    int expected = action.sa_handler != SIG_IGN;
    e->host_signal_expected[i] = expected;
    atomic_fetch_add(&domain_expected_host_signals, expected);
    if (c->index == 0) {
      printf(
          "HOST_SIGNAL_PROBE {\"signal\":%d,\"ignored\":%d,\"blocked\":%d}\n",
          probes[i], !expected, sigismember(&mask, probes[i]));
    }
  }
#endif
  e->stack_low = (uintptr_t)stack_alloc(2 * 1024 * 1024);
  e->stack_high = e->stack_low + 2 * 1024 * 1024;
  int error = owner_command(c, ADOPT_ALLOC);
  if (error) {
    fprintf(stderr, "domain allocation: %d\n", error);
    _exit(1);
  }
  e->guest.pc = (uintptr_t)domain_start;
#if defined(__x86_64__)
  e->guest.saved[2] = (uintptr_t)c;             /* r12 */
  e->guest.saved[3] = (uintptr_t)control.entry; /* r13 */
  __asm__ volatile("stmxcsr %0" : "=m"(e->guest.saved[6]));
  __asm__ volatile("fnstcw %0" : "=m"(e->guest.saved[7]));
#else
  e->guest.saved[0] = (uintptr_t)c;             /* x19 */
  e->guest.saved[1] = (uintptr_t)control.entry; /* x20 */
#endif
}

static void *carrier_thread(void *argument) {
  struct carrier *c = argument;
  struct exchange *e = &c->exchange;
#ifdef GW_DOMAIN_HOOKS
  if (experiment_thread_enter()) {
    _exit(101);
  }
#endif
  prepare_carrier(c);
#ifdef GW_DOMAIN_SIGNALS
  e->fault = domain_fault;
  e->fault_page = fault_page;
  struct signal_slot *slot = &signal_slots[c->index];
  signal_register(slot, e);
  e->fault(e->fault_page);
#endif
  CHECK(e, pthread_setspecific(host_key, c) == 0);
  pthread_barrier_wait(&allocated);
  void *clear_before = NULL, *clear_after = NULL;
  int before = prctl(PR_GET_TID_ADDRESS, &clear_before, 0, 0, 0);
  CHECK(e, fesetround(FE_DOWNWARD) == 0);
  errno = EDOM;
  e->swap(&e->host, &e->guest);
  host_check(e);
  while (e->state == ADOPT_RECOVERY) {
    /* The fixture deliberately retains the suspended Guest stack and uses a
     * separate entry stack with its own TCB. This is a controlled C-only
     * transfer, not a way to abandon arbitrary Host or VM frames. */
    struct domain transfer = e->guest;
    transfer.sp = (uintptr_t)c->recovery_stack & ~(uintptr_t)15;
    transfer.pc = (uintptr_t)domain_start;
#if defined(__x86_64__)
    transfer.saved[2] = (uintptr_t)c->recovery_argument;
    transfer.saved[3] = (uintptr_t)c->recovery_entry;
#else
    transfer.saved[0] = (uintptr_t)c->recovery_argument;
    transfer.saved[1] = (uintptr_t)c->recovery_entry;
#endif
    ++c->recovery_transfers;
    e->swap(&e->host, &transfer);
    host_check(e);
  }
  CHECK(e, c->recovery_transfers == 4 && c->recovery_completions == 4);
  CHECK(e, e->state == DOMAIN_READY && e->guest_tid == e->host_tid);
  CHECK(e, e->guest.tp != e->host.tp && e->guest_tls != e->host_tls);
  for (int i = 0; i < ADOPT_ROUNDS; ++i) {
    CHECK(e, invoke(e) == 84);
  }
  CHECK(e, owner_command(c, ADOPT_FREE) == EBUSY);
  errno = EDOM;
  e->state = ADOPT_STOP;
  e->swap(&e->host, &e->guest);
  host_check(e);
  CHECK(e, e->state == ADOPT_STOPPED && c->guest_destructors == 1 &&
               c->tls_destructors == 1 && c->cleanup_handlers == 1);
  int after = prctl(PR_GET_TID_ADDRESS, &clear_after, 0, 0, 0);
  if (!before && !after) {
    CHECK(e, clear_before == clear_after);
  }
#ifdef GW_DOMAIN_SIGNALS
  atomic_fetch_add(&finished_carriers, 1);
  pthread_barrier_wait(&signals_finished);
  signal_unregister(slot);
  CHECK(e,
        atomic_load(&slot->failures) == 0 && atomic_load(&slot->faults) == 2);
  CHECK(e, atomic_load(&slot->signals) >= 2);
#endif
  CHECK(e, owner_command(c, ADOPT_FREE) == 0);
  size_t page = (size_t)sysconf(_SC_PAGESIZE);
  CHECK(e, munmap((void *)(e->stack_low - page),
                  e->stack_high - e->stack_low + 2 * page) == 0);
#ifdef GW_DOMAIN_HOOKS
  CHECK(e, experiment_thread_leave() == 0);
#endif
  return c;
}

static void start_owner(char **argv) {
#ifdef GW_DOMAIN_CHAIN
  control.note_guest_thread = signal_note_guest_thread;
  control.host_signals = signal_router_reserved();
#endif
  control.dso = argv[4];
  char pointer[32];
  snprintf(pointer, sizeof pointer, "%lx", (unsigned long)(uintptr_t)&control);
  char *preload = getenv("GW_DOMAIN_PRELOAD");
  if (control.host_signals && preload && *preload) {
    /* Guest ownership is configured in main, before loading test DSOs. A
     * preloaded constructor would run before that boundary. */
    fprintf(stderr, "Guest preload precedes signal ownership setup\n");
    exit(110);
  }
  char *arguments[7] = {argv[1], "--library-path", argv[2]};
  size_t count = 3;
  if (preload && *preload) {
    arguments[count++] = "--preload";
    arguments[count++] = preload;
  }
  arguments[count++] = argv[3];
  arguments[count++] = pointer;
  struct gw_bootstrap_image loader = gw_bootstrap_map(argv[1]);
  void *stack = stack_alloc(2 * 1024 * 1024);
  void *sp =
      gw_bootstrap_stack(loader, stack, 2 * 1024 * 1024, arguments, count);
  long tid = launch_guest(CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
                              CLONE_THREAD | CLONE_SYSVSEM,
                          sp, loader.entry);
  if (tid <= 0) {
    fprintf(stderr, "owner clone: %ld\n", tid);
    exit(1);
  }
  while (!atomic_load_explicit(&control.ready, memory_order_acquire)) {
    adopt_wait(&control.ready, 0);
  }
}

int main(int argc, char **argv) {
  if (argc != 5) {
    return 2;
  }
#ifdef GW_DOMAIN_CHAIN
  if (signal_router_init()) {
    return 104;
  }
#endif
#ifdef GW_DOMAIN_HOOKS
  if (experiment_start()) {
    return 100;
  }
#endif
#ifdef GW_DOMAIN_CHAIN
  if (signal_router_lease_guest_nptl()) {
    return 104;
  }
#endif
  start_owner(argv);
#ifdef GW_DOMAIN_SIGNALS
  signal_install();
  fault_page = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (fault_page == MAP_FAILED ||
      pthread_barrier_init(&signals_finished, NULL, ADOPT_THREADS + 1)) {
    return 1;
  }
#endif
  if (pthread_key_create(&host_key, host_destruct) ||
      pthread_barrier_init(&allocated, NULL, ADOPT_THREADS)) {
    return 1;
  }
  int failures = 0, calls = 0, destructors = 0, tls_destructors = 0, exits = 0;
  int signal_events = 0, signal_faults = 0, inherited_signal_masks = 0;
  int recovery_transfers = 0, recovery_completions = 0;
  for (int batch = 0; batch < ADOPT_BATCHES; ++batch) {
#ifdef GW_DOMAIN_SIGNALS
    atomic_store(&finished_carriers, 0);
    for (int i = 0; i < ADOPT_THREADS; ++i) {
      atomic_store(&signal_slots[i].signals, 0);
      atomic_store(&signal_slots[i].faults, 0);
      atomic_store(&signal_slots[i].failures, 0);
    }
#endif
    struct carrier carriers[ADOPT_THREADS] = {0};
    pthread_t threads[ADOPT_THREADS];
    for (int i = 0; i < ADOPT_THREADS; ++i) {
      carriers[i].index = i;
      carriers[i].exit_mode = i % 2;
      if (pthread_create(&threads[i], NULL, carrier_thread, &carriers[i])) {
        _exit(1);
      }
    }
#ifdef GW_DOMAIN_SIGNALS
    while (atomic_load(&finished_carriers) != ADOPT_THREADS) {
      signal_send();
      struct timespec pause = {.tv_nsec = 100000};
      nanosleep(&pause, NULL);
    }
    pthread_barrier_wait(&signals_finished);
#endif
    for (int i = 0; i < ADOPT_THREADS; ++i) {
      void *result;
      if (pthread_join(threads[i], &result) || result != &carriers[i]) {
        _exit(1);
      }
      struct exchange *e = &carriers[i].exchange;
      CHECK(e, e->calls == ADOPT_ROUNDS * DOMAIN_NESTING &&
                   e->callbacks == e->calls);
      failures += e->failures;
      calls += e->calls;
      destructors += carriers[i].guest_destructors;
      tls_destructors += carriers[i].tls_destructors;
      exits += carriers[i].run_status;
      recovery_transfers += carriers[i].recovery_transfers;
      recovery_completions += carriers[i].recovery_completions;
#ifdef GW_DOMAIN_SIGNALS
      signal_events += atomic_load(&signal_slots[i].signals);
      signal_faults += atomic_load(&signal_slots[i].faults);
      inherited_signal_masks += signal_slots[i].inherited_blocked;
#endif
      for (int j = 0; j < i; ++j) {
        failures += e->guest.tp == carriers[j].exchange.guest.tp;
      }
    }
  }
  failures += atomic_load(&host_destructors) != ADOPT_THREADS * ADOPT_BATCHES;
#ifdef GW_DOMAIN_HOOKS
  failures += experiment_finish();
#endif
#ifdef GW_DOMAIN_CHAIN
  int signal_restore = signal_router_restore();
  failures += signal_restore != 0;
  printf("SIGNAL_ROUTER {\"registrations\":%u,\"restore_error\":%d}\n",
         signal_router_registrations(), signal_restore);
#endif
  printf(
      "{\"result\":\"%s\",\"uid\":%u,\"failures\":%d,\"calls\":%d,"
      "\"carriers\":%d,\"guest_destructors\":%d,\"host_destructors\":%d,"
      "\"tls_destructors\":%d,\"guest_pthread_exits\":%d,"
      "\"signal_events\":%d,\"signal_faults\":%d,"
      "\"guest_recovery_transfers\":%d,"
      "\"guest_recovery_completions\":%d,"
      "\"inherited_blocked_signal_masks\":%d,"
      "\"host_join_completed\":true}\n",
      failures ? "FAIL" : "PASS", (unsigned)getuid(), failures, calls,
      ADOPT_THREADS * ADOPT_BATCHES, destructors,
      atomic_load(&host_destructors), tls_destructors, exits, signal_events,
      signal_faults, recovery_transfers, recovery_completions,
      inherited_signal_masks);
  fflush(stdout);
  /* The bootstrap owner and process runtime intentionally outlive carriers. */
  _exit(failures ? 1 : 0);
}
