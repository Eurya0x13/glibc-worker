#define _GNU_SOURCE
#include "common.h"
#include "nonlocal-mask.h"
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <fenv.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>

static struct async_control *control;
static int (*allocate_domain)(void *, size_t, void **, void **, void **);
static int (*bind_domain)(void *);
static int (*run_domain)(void (*)(void *), void *);
static int (*finish_domain)(void);
static int (*free_domain)(void *);
static __thread int guest_tls = 41;
static __thread struct async_carrier *current;
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

static int notify(void *argument, struct gw_signal_receiver *receiver) {
  struct async_carrier *carrier = argument;
  assert(receiver == &carrier->receiver);
  return syscall(SYS_tgkill, getpid(), carrier->exchange.host_tid,
                 control->notification)
             ? errno
             : 0;
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

  siginfo_t packet;
  enum gw_signal_queue_source source;
  int selected = 0;
  while (gw_signal_inbox_deliver(&carrier->receiver, &packet, &source)) {
    assert(packet.si_signo == TEST_LOGICAL_SIGNAL &&
           packet.si_code == SI_QUEUE);
    assert(source == GW_SIGNAL_THREAD && packet.si_pid == getpid());
    assert(packet.si_uid == getuid());
    assert(packet.si_value.sival_int ==
           carrier->index * 1000 + ++carrier->expected);
    int *memory = malloc(sizeof *memory);
    assert(memory);
    *memory = packet.si_value.sival_int;
    assert(*memory == carrier->index * 1000 + carrier->expected);
    free(memory);
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
  errno = saved_errno;
  return 0;
}

static void serve(void *argument) {
  struct async_carrier *carrier = argument;
  struct exchange *exchange = &carrier->exchange;
  current = carrier;
  carrier->busy_begin = (uintptr_t)gw_async_busy_begin;
  carrier->busy_end = (uintptr_t)gw_async_busy_end;
  assert(guest_tls == 41);
  assert(!fesetround(FE_UPWARD));
  carrier->entry =
      (struct domain_signal_call){.entry = receive,
                                  .tp = (uintptr_t)__builtin_thread_pointer(),
                                  .number = control->notification,
                                  .info = carrier};
#if defined(__x86_64__)
  __asm__ volatile("stmxcsr %0" : "=m"(carrier->entry.control[0]));
  __asm__ volatile("fnstcw %0" : "=m"(carrier->entry.control[1]));
#else
  __asm__ volatile("mrs %0, fpcr" : "=r"(carrier->entry.control[0]));
  __asm__ volatile("mrs %0, fpsr" : "=r"(carrier->entry.control[1]));
  __asm__ volatile("mov %0, x18" : "=r"(carrier->entry.x18));
#endif
  errno = EBADF;
  exchange->swap(&exchange->guest, &exchange->host);
  /* Host has not polled or entered Guest during PHASE_BUSY/PHASE_WAIT. This
   * later call tests native Guest interruption using the same physical wake. */
  assert(atomic_load(&carrier->phase) == PHASE_GUEST);
  gw_nonlocal_mask_restore(exchange->host_execution_mask);
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
  assert(capabilities() == 0x4757440100000006UL);
  RESOLVE(allocate_domain, "__gw_domain_alloc");
  RESOLVE(bind_domain, "__gw_domain_bind");
  RESOLVE(run_domain, "__gw_domain_run");
  RESOLVE(finish_domain, "__gw_domain_finish");
  RESOLVE(free_domain, "__gw_domain_free");
#undef RESOLVE
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
  assert(TEST_LOGICAL_SIGNAL >= SIGRTMIN && TEST_LOGICAL_SIGNAL <= SIGRTMAX);
  gw_signal_inbox_init(&control->inbox, 0, 128);
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
        assert(
            !gw_signal_receiver_open(&control->inbox, &carrier->receiver, 0));
        break;
      }
      case OWNER_SEND: {
        siginfo_t packet = {.si_signo = TEST_LOGICAL_SIGNAL,
                            .si_code = SI_QUEUE};
        packet.si_pid = getpid();
        packet.si_uid = getuid();
        packet.si_value.sival_int = control->value;
        struct gw_signal_publication sent = gw_signal_inbox_put(
            &control->inbox, &carrier->receiver, &packet, notify, carrier);
        control->error = sent.status;
        assert(!sent.wake_error);
        break;
      }
      case OWNER_CLOSE:
        gw_signal_receiver_close(&carrier->receiver);
        atomic_store_explicit(&carrier->closed, 1, memory_order_release);
        control->error = 0;
        break;
      case OWNER_EMPTY:
        control->error = atomic_load(&carrier->closed)
                             ? ESRCH
                             : notify(carrier, &carrier->receiver);
        break;
      case OWNER_FREE:
        assert(!gw_signal_receiver_destroy(&carrier->receiver));
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
