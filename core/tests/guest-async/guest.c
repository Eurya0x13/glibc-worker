#define _GNU_SOURCE
#include "common.h"
#include "nonlocal-mask.h"
#include "send-lifetime.h"
#include "private-threads.h"
#include "mask-inheritance.h"
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <fenv.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <unistd.h>

static struct async_control *control;
static int (*allocate_domain)(void *, size_t, void **, void **, void **);
static int (*bind_domain)(void *);
static int (*run_domain)(void (*)(void *), void *);
static int (*finish_domain)(void);
static int (*free_domain)(void *);
static __thread int guest_tls = 41;
static __thread struct async_carrier *current;
static struct async_carrier *recipients[TEST_CARRIERS];
extern char gw_async_busy_begin[], gw_async_busy_end[];

static __attribute__((noinline, noclone)) void guest_busy(
    struct async_carrier *carrier) {
  __asm__ volatile(".global gw_async_busy_begin\ngw_async_busy_begin:" ::
                       : "memory");
  atomic_store_explicit(&carrier->guest_running, 1, memory_order_release);
  while (atomic_load_explicit(&carrier->received, memory_order_acquire) <
         TEST_TOTAL_PACKETS) {
    __asm__ volatile("" ::: "memory");
  }
  atomic_store_explicit(&carrier->guest_running, 0, memory_order_release);
  __asm__ volatile(".global gw_async_busy_end\ngw_async_busy_end:" ::
                       : "memory");
}

static int notify(void *argument, struct gw_signal_target *target) {
  assert(argument == control);
  struct async_carrier *carrier = target->context;
  assert(target == carrier->target &&
         target->tid == carrier->exchange.host_tid);
  return syscall(SYS_tgkill, getpid(), target->tid, control->notification)
             ? errno
             : 0;
}

static int send_signal(void *thread, int tid, const siginfo_t *information,
                       int *handled) {
  if (test_send_lifetime_probe(thread, tid, information, handled)) {
    return 0;
  }
  if (information->si_signo != TEST_LOGICAL_SIGNAL &&
      information->si_signo != 50 && information->si_signo != 0) {
    *handled = 0; /* Keep assertion/abort diagnostics native in this fixture. */
    return 0;
  }
  *handled = 1;
  struct gw_signal_publication sent = gw_signal_targets_send(
      &control->targets, (uintptr_t)thread, tid, information);
  /* Accepted-but-unnotified work is distinct from an unsent packet. */
  assert(!sent.wake_error);

  errno = ERANGE; /* Owning libc must preserve pthread callers' errno. */
  return sent.status;
}

static void select_carrier(struct async_carrier *carrier) {
  uint64_t previous = gw_nonlocal_mask_block();
  /* Only the oracle chooses one particular recipient. The runtime callback
   * itself uses the dynamic registry's native shared-target selection. */
  for (int i = 0; i < TEST_CARRIERS; ++i) {
    struct async_carrier *other = recipients[i];
    if (other && other->target && !atomic_load(&other->closed)) {
      struct gw_signal_publication result = gw_signal_target_mask(
          other->target, other == carrier ? 0 : UINT64_MAX);
      assert(!result.status && !result.wake_error);
    }
  }
  gw_nonlocal_mask_restore(previous);
}

/* This is the internal, normally returning entry under test, not a public
 * sigaction ABI. In particular, it receives no foreign kernel ucontext. A
 * production Guest disposition/context adapter must be layered above it. */
static int receive(int number, void *information, void *context) {
  struct async_carrier *carrier = information;
  assert(number == control->notification && !context && carrier == current);
  int saved_errno = errno;
  assert(guest_tls == 41 && saved_errno == EBADF);
  assert((uintptr_t)__builtin_thread_pointer() == carrier->exchange.guest.tp);
  assert(syscall(SYS_gettid) == carrier->exchange.host_tid);
  char local;
  assert((uintptr_t)&local >= (uintptr_t)carrier->entry_stack &&
         (uintptr_t)&local < (uintptr_t)carrier->entry_stack + 65536);
  assert(fegetround() == FE_UPWARD);

  int phase = atomic_load(&carrier->phase);
  if (carrier->interrupted_host_frame &&
      (phase == PHASE_BUSY || phase == PHASE_HOST_INTERRUPT ||
       phase == PHASE_VISIBLE_FUTEX)) {
    /* Deliberately bypass tagged sends: this is a real Host-owned signal
     * arriving while Guest code executes in a Host-origin async entry. */
    uint64_t bit = UINT64_C(1) << (TEST_NESTED_HOST_SIGNAL - 1), previous;
    assert(
        !syscall(SYS_rt_sigprocmask, SIG_UNBLOCK, &bit, &previous, sizeof bit));
    int count = carrier->nested_host_count;
    assert(!syscall(SYS_tgkill, getpid(), syscall(SYS_gettid),
                    TEST_NESTED_HOST_SIGNAL));
    assert(carrier->nested_host_count == count + 1);
    assert(!syscall(SYS_rt_sigprocmask, SIG_SETMASK, &previous, NULL,
                    sizeof previous));
    assert(errno == saved_errno && guest_tls == 41);
    assert(fegetround() == FE_UPWARD);
  }

  siginfo_t packet;
  enum gw_signal_queue_source source;
  int selected = 0;
  while (gw_signal_target_deliver(carrier->target, &packet, &source)) {
    assert(packet.si_signo == TEST_LOGICAL_SIGNAL);
    assert(packet.si_pid == getpid());
    assert(packet.si_uid == getuid());
    if (carrier->self_phase) {
      int expected = carrier->self_received++;
      assert(packet.si_code == (expected == 3 || expected == 5 ? SI_QUEUE
                                : expected == 4                ? SI_USER
                                                               : SI_TKILL));
      assert(source == (expected >= 4 ? GW_SIGNAL_PROCESS : GW_SIGNAL_THREAD));
      if (packet.si_code == SI_QUEUE) {
        assert(packet.si_value.sival_int == 9917);
      }
      ++selected;
      continue;
    }
    int expected = ++carrier->expected;
    assert(packet.si_code == (expected == 3   ? SI_TKILL
                              : expected == 6 ? SI_USER
                                              : SI_QUEUE));
    assert(source == (expected == 2 || expected == 6 ? GW_SIGNAL_PROCESS
                                                     : GW_SIGNAL_THREAD));
    if (packet.si_code == SI_QUEUE) {
      assert(packet.si_value.sival_int == carrier->index * 1000 + expected);
    }
    volatile int *memory = malloc(sizeof *memory);
    assert(memory);
    *memory = packet.si_code == SI_QUEUE ? packet.si_value.sival_int
                                         : carrier->index * 1000 + expected;
    assert(*memory == carrier->index * 1000 + carrier->expected);
    free((void *)memory);
    ++selected;
    atomic_fetch_add_explicit(&carrier->received, 1, memory_order_release);
  }
  if (!selected) {
    atomic_fetch_add_explicit(&carrier->empty_notifications, 1,
                              memory_order_release);
  }
  if (carrier->delay && selected) {
    struct timespec work = {.tv_nsec = 150000000};
    assert(!syscall(SYS_clock_nanosleep, CLOCK_MONOTONIC, 0, &work, NULL));
    ++carrier->completed_work;
  }
  /* Deliberately perturb only Guest state; both return domains must retain
   * their own errno and FP control after the entry gateway returns. */
  assert(!fesetround(FE_TONEAREST));
  errno = carrier->self_errno ? EILSEQ : saved_errno;
  return 0;
}

static int execute_receive(void *argument) {
  struct test_execution *execution = argument;
  struct async_carrier *carrier = execution->carrier;
  assert(carrier == current && guest_tls == 41 && errno == EBADF);
  assert(!execution->pinned && !execution->finished);
  assert(!gw_signal_target_pin(carrier->target));
  execution->pinned = 1;

  int phase = atomic_load_explicit(&carrier->phase, memory_order_acquire);
  if (phase == PHASE_RECOVER || phase == PHASE_RECOVER_AFTER) {
    if (phase == PHASE_RECOVER) {
      execution->recovery_depth = ++carrier->recovery_depth;
    }
    siginfo_t packet;
    enum gw_signal_queue_source source;
    assert(gw_signal_target_deliver(carrier->target, &packet, &source) ==
           TEST_LOGICAL_SIGNAL);
    assert(
        source == GW_SIGNAL_THREAD && packet.si_code == SI_QUEUE &&
        packet.si_value.sival_int ==
            carrier->index * 1000 + TEST_RECOVERY_VALUE +
                (phase == PHASE_RECOVER ? execution->recovery_depth - 1 : 0));
    if (phase == PHASE_RECOVER) {
      uint64_t bit = UINT64_C(1) << (TEST_NESTED_HOST_SIGNAL - 1);
      assert(!syscall(SYS_rt_sigprocmask, SIG_UNBLOCK, &bit, NULL, sizeof bit));
      assert(!syscall(SYS_tgkill, getpid(), syscall(SYS_gettid),
                      TEST_NESTED_HOST_SIGNAL));
      assert(!"Host jump must abandon this Guest invocation");
    }
    return 0;
  }
  return receive(control->notification, carrier, NULL);
}

static void finish_receive(void *argument, enum gw_signal_exit reason) {
  struct test_execution *execution = argument;
  struct async_carrier *carrier = execution->carrier;
  assert(carrier == current && guest_tls == 41);
  assert(execution->pinned && !execution->finished);
  uint64_t mask;
  assert(!syscall(SYS_rt_sigprocmask, SIG_SETMASK, NULL, &mask, sizeof mask));
  assert(mask == (UINT64_MAX & ~(UINT64_C(1) << (SIGKILL - 1)) &
                  ~(UINT64_C(1) << (SIGSTOP - 1))));
  assert(!gw_signal_target_unpin(carrier->target));
  execution->pinned = 0;
  execution->finished = 1;
  if (reason == GW_SIGNAL_HOST_JUMP) {
    assert(execution->recovery_depth == carrier->recovery_depth);
    --carrier->recovery_depth;
    atomic_fetch_add_explicit(&carrier->abandoned, 1, memory_order_release);
  } else if (atomic_load(&carrier->phase) == PHASE_RECOVER_AFTER) {
    atomic_fetch_add_explicit(&carrier->recovered, 1, memory_order_release);
  }
}

static void prepare_executions(struct async_carrier *carrier) {
  for (int i = 0; i < TEST_EXECUTIONS; ++i) {
    struct test_execution *execution = &carrier->executions[i];
    execution->carrier = carrier;
    struct gw_signal_execution_config config = {
        .stack = execution->stack,
        .stack_size = 65536,
        .abort_stack = execution->abort_stack,
        .abort_stack_size = 65536,
        .alternate = {.ss_sp = execution->stack, .ss_size = 65536},
        .swap = carrier->exchange.swap,
        .invoke = execute_receive,
        .finish = finish_receive,
        .argument = execution};
    if (!i) {
      struct gw_signal_execution_config invalid = config;
      invalid.abort_stack = config.stack;
      assert(gw_guest_signal_execution_init(&execution->scope, &invalid) ==
             EINVAL);
      invalid = config;
      invalid.alternate.ss_sp = config.abort_stack;
      assert(gw_guest_signal_execution_init(&execution->scope, &invalid) ==
             EINVAL);
    }
    assert(!gw_guest_signal_execution_init(&execution->scope, &config));
  }
}

static void self_sends(struct async_carrier *carrier) {
  select_carrier(carrier);
  union sigval value = {.sival_int = 9917};
  /* A real Host private handler can interrupt the preceding native Guest
   * futex. Establish the errno input for these public send API assertions
   * after that wait, rather than assuming the raw syscall never saw EINTR. */
  errno = EBADF;
  carrier->self_phase = 1;
  assert(!raise(TEST_LOGICAL_SIGNAL) && carrier->self_received == 1);
  assert(!gsignal(TEST_LOGICAL_SIGNAL) && carrier->self_received == 2);
  carrier->self_errno = 1;
  assert(!pthread_kill(pthread_self(), TEST_LOGICAL_SIGNAL) &&
         carrier->self_received == 3);
  assert(errno == EILSEQ);
  carrier->self_errno = 0;
  errno = EBADF;
  assert(!pthread_sigqueue(pthread_self(), TEST_LOGICAL_SIGNAL, value) &&
         carrier->self_received == 4);
  assert(!kill(getpid(), TEST_LOGICAL_SIGNAL) && carrier->self_received == 5);
  carrier->self_errno = 1;
  assert(!sigqueue(getpid(), TEST_LOGICAL_SIGNAL, value) &&
         carrier->self_received == 6);
  assert(errno == EILSEQ);
  carrier->self_errno = 0;
  errno = EBADF;
  carrier->self_phase = 0;

  assert(!pthread_kill(pthread_self(), 0) && errno == EBADF);
  assert(pthread_kill(pthread_self(), 32) == EINVAL && errno == EBADF);
  assert(pthread_sigqueue(pthread_self(), 33, value) == EINVAL &&
         errno == EBADF);
  assert(pthread_kill(pthread_self(), -1) == EINVAL && errno == EBADF);
  assert(pthread_sigqueue(pthread_self(), 50, value) == EBUSY &&
         errno == EBADF);
  assert(raise(50) == -1 && errno == EBUSY);
  assert(sigqueue(getpid(), 50, value) == -1 && errno == EBUSY);
  assert(kill(getpid(), 50) == -1 && errno == EBUSY);
  errno = EBADF;
}

static void serve(void *argument) {
  struct async_carrier *carrier = argument;
  struct exchange *exchange = &carrier->exchange;
  current = carrier;
  test_adopted_mask_inheritance();
  carrier->busy_begin = (uintptr_t)gw_async_busy_begin;
  carrier->busy_end = (uintptr_t)gw_async_busy_end;
  assert(guest_tls == 41);
  assert(!fesetround(FE_UPWARD));
  assert(!gw_signal_target_open(&control->targets, (uintptr_t)carrier->token,
                                exchange->host_tid, UINT64_MAX, carrier,
                                &carrier->target));
  prepare_executions(carrier);
  errno = EBADF;
  exchange->swap(&exchange->guest, &exchange->host);
  /* Host has not polled or entered Guest during PHASE_BUSY/PHASE_WAIT. This
   * later call tests native Guest interruption using the same physical wake. */
  assert(atomic_load(&carrier->phase) == PHASE_GUEST);
  gw_nonlocal_mask_restore(exchange->guest_execution_mask);
  if (control->gnu_host) {
    uint64_t mask;
    assert(!syscall(SYS_rt_sigprocmask, SIG_SETMASK, NULL, &mask, sizeof mask));
    assert(mask & (UINT64_C(1) << 31));
    assert(!(mask & (UINT64_C(1) << 32)));
  }
  while (!atomic_load_explicit(&carrier->self_allowed, memory_order_acquire)) {
    wait_word(&carrier->self_allowed, 0);
  }
  self_sends(carrier);
  guest_busy(carrier);
  assert(guest_tls == 41 && errno == EBADF && fegetround() == FE_UPWARD);
  gw_nonlocal_mask_block();
  atomic_store_explicit(&carrier->phase, PHASE_DONE, memory_order_release);
  exchange->swap(&exchange->guest, &exchange->host);
  assert(atomic_load(&carrier->closed));
  current = NULL;
}

static void adopted_entry(void *argument) {
  struct async_carrier *carrier = argument;
  assert(!bind_domain(carrier->token));
  assert(!run_domain(serve, carrier));
  assert(!finish_domain());
  carrier->exchange.swap(&carrier->exchange.guest, &carrier->exchange.host);
  _exit(111);
}

static void resolve(void) {
#define RESOLVE(variable, symbol)                      \
  do {                                                 \
    *(void **)&variable = dlsym(RTLD_DEFAULT, symbol); \
    assert(variable);                                  \
  } while (0)
  unsigned long (*capabilities)(void);
  RESOLVE(capabilities, "__gw_domain_capabilities");
  assert(capabilities() == 0x4757440100000012UL);
  RESOLVE(allocate_domain, "__gw_domain_alloc");
  RESOLVE(bind_domain, "__gw_domain_bind");
  RESOLVE(run_domain, "__gw_domain_run");
  RESOLVE(finish_domain, "__gw_domain_finish");
  RESOLVE(free_domain, "__gw_domain_free");
  int (*send_hook)(unsigned, int (*)(void *, int, const siginfo_t *, int *));
  int (*thread_hook)(void (*)(int));
  RESOLVE(send_hook, "__gw_domain_signal_send_hook");
  RESOLVE(thread_hook, "__gw_domain_thread_hook");
  test_private_threads();
  assert(!pthread_kill(pthread_self(), 0));
  assert(!kill(getpid(), 0));
  test_send_lifetimes(send_hook, thread_hook, send_signal);
  control->send_lifecycle = 1;
#undef RESOLVE
}

static int owner_send(struct async_carrier *carrier, int value) {
  if (!atomic_load(&carrier->closed)) {
    select_carrier(carrier);
  }
  int packet = value - carrier->index * 1000;
  int result;
  errno = ECHILD;
  if (packet == 2) {
    result = sigqueue(getpid(), TEST_LOGICAL_SIGNAL,
                      (union sigval){.sival_int = value});
  } else if (packet == 3) {
    result = pthread_kill((pthread_t)carrier->token, TEST_LOGICAL_SIGNAL);
  } else if (packet == 6) {
    result = kill(getpid(), TEST_LOGICAL_SIGNAL);
  } else {
    result = pthread_sigqueue((pthread_t)carrier->token, TEST_LOGICAL_SIGNAL,
                              (union sigval){.sival_int = value});
  }
  assert(errno == ECHILD);
  return result;
}

static void check_embedded_heap(void) {
  void *process_break = sbrk(0);
  assert(process_break != (void *)-1);
  assert(sbrk(1) == (void *)-1 && errno == ENOMEM);
  assert(brk((void *)((uintptr_t)process_break + 1)) == -1 && errno == ENOMEM);

  volatile unsigned char *allocation = malloc(32768);
  assert(allocation);
  allocation[0] = 41;
  allocation[32767] = 73;
  assert(allocation[0] == 41 && allocation[32767] == 73);
  free((void *)allocation);
}

int main(int argc, char **argv) {
  assert(argc == 2);
  control = (void *)(uintptr_t)strtoull(argv[1], NULL, 16);
  uint32_t layout[TEST_LAYOUT_WORDS];
  test_layout(layout);
  for (int i = 0; i < TEST_LAYOUT_WORDS; ++i) {
    assert(layout[i] == control->layout[i]);
  }
  resolve();
  check_embedded_heap();
  assert(TEST_LOGICAL_SIGNAL >= SIGRTMIN && TEST_LOGICAL_SIGNAL <= SIGRTMAX);
  gw_signal_targets_init(&control->targets, UINT64_C(1) << 49, 128, notify,
                         control);
  control->entry = adopted_entry;
  atomic_store_explicit(&control->ready, 1, memory_order_release);
  wake(&control->ready);

  int sequence = 0;
  for (;;) {
    int request = atomic_load_explicit(&control->request, memory_order_acquire);
    if (request == sequence) {
      wait_word(&control->request, sequence);
      continue;
    }
    struct async_carrier *carrier = control->carrier;
    uint64_t previous = gw_nonlocal_mask_block();
    switch (control->operation) {
      case OWNER_ALLOC: {
        void *tp = NULL, *top = NULL;
        struct exchange *exchange = &carrier->exchange;
        control->error =
            allocate_domain((void *)exchange->stack_low,
                            exchange->stack_high - exchange->stack_low,
                            &carrier->token, &tp, &top);
        assert(!control->error);
        exchange->guest.tp = (uintptr_t)tp;
        exchange->guest.sp = (uintptr_t)top;
        recipients[carrier->index % TEST_CARRIERS] = carrier;
        break;
      }
      case OWNER_SEND: {
        control->error = owner_send(carrier, control->value);
        break;
      }
      case OWNER_RECOVER: {
        select_carrier(carrier);
        assert(!gw_signal_target_mask(carrier->target, UINT64_MAX).status);
        for (int i = 0; i < TEST_RECOVERY_DEPTH; ++i) {
          union sigval value = {.sival_int = carrier->index * 1000 +
                                             TEST_RECOVERY_VALUE + i};
          assert(!pthread_sigqueue((pthread_t)carrier->token,
                                   TEST_LOGICAL_SIGNAL, value));
        }
        control->error = gw_signal_target_mask(carrier->target, 0).status;
        break;
      }
      case OWNER_CLOSE:
        gw_signal_target_close(carrier->target);
        atomic_store_explicit(&carrier->closed, 1, memory_order_release);
        control->error = 0;
        break;
      case OWNER_EMPTY:
        control->error = atomic_load(&carrier->closed)
                             ? ESRCH
                             : notify(control, carrier->target);
        break;
      case OWNER_FREE:
        assert(!gw_signal_target_release(carrier->target));
        control->error = free_domain(carrier->token);
        break;
      default:
        _exit(111);
    }
    gw_nonlocal_mask_restore(previous);
    sequence = request;
    atomic_store_explicit(&control->response, sequence, memory_order_release);
    wake(&control->response);
  }
}
