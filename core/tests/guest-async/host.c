#define _GNU_SOURCE
#include "common.h"
#include "loader_image.h"
#include "nonlocal-mask.h"
#include "signal-router.h"
#include "signal-select.h"
#include "signal-stack.h"
#include "signals.h"
#include <assert.h>
#include <errno.h>
#include <fenv.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <poll.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <ucontext.h>

extern long launch_guest(unsigned long, void *, void *);
static struct async_control control;
static struct async_carrier carriers[TEST_CARRIERS];
static _Thread_local struct async_carrier *current;
static _Thread_local int host_tls = 17;
#ifdef __ANDROID__
static _Atomic int private_callbacks;

static int private_callback(void *argument) {
  assert(argument == &private_callbacks && host_tls == 17);
  if (current) {
    assert(atomic_load(&current->phase) == PHASE_GUEST);
    assert((uintptr_t)__builtin_thread_pointer() == current->exchange.host.tp);
  }
  atomic_fetch_add_explicit(&private_callbacks, 1, memory_order_relaxed);
  return 1;
}

static void private_host_routes(void) {
  int (*run_on_all)(int (*)(void *), void *) =
      dlsym(RTLD_NEXT, "android_run_on_all_threads");
  assert(run_on_all);
  for (int i = 0; i < TEST_CARRIERS; ++i) {
    while (atomic_load_explicit(&carriers[i].phase, memory_order_acquire) !=
           PHASE_GUEST) {
      sched_yield();
    }
  }
  int previous = atomic_load(&private_callbacks);
  /* This native Bionic API registers/restores its own private signal through
   * libc's PLT and invokes a handler on both adopted GNU carriers. Neither
   * the registration nor the send is performed by this test's interposer.
   * https://android.googlesource.com/platform/bionic/+/android13-release/libc/bionic/pthread_internal.cpp
   */
  assert(run_on_all(private_callback, &private_callbacks));
  assert(atomic_load(&private_callbacks) == previous + TEST_CARRIERS + 1);
}
#endif

/* LP64 kernel ABI, not either libc's public struct sigaction. Public GNU
 * queries intentionally reject 32/33; inspect the actual shared dispositions.
 * https://github.com/torvalds/linux/blob/v6.12/include/uapi/asm-generic/signal.h
 */
struct kernel_action {
  uintptr_t handler, flags, restorer;
  uint64_t mask;
};

static void snapshot_private(struct kernel_action actions[9]) {
  for (int number = 32; number <= 40; ++number) {
    actions[number - 32] = (struct kernel_action){0};
    assert(!syscall(SYS_rt_sigaction, number, NULL, &actions[number - 32],
                    sizeof(uint64_t)));
  }
}

static void *stack_alloc(size_t size) {
  size_t page = (size_t)sysconf(_SC_PAGESIZE);
  char *stack = mmap(NULL, size + 2 * page, PROT_NONE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(stack != MAP_FAILED);
  assert(!mprotect(stack + page, size, PROT_READ | PROT_WRITE));
  return stack + page;
}

static void stack_free(void *stack, size_t size) {
  size_t page = (size_t)sysconf(_SC_PAGESIZE);
  assert(!munmap((char *)stack - page, size + 2 * page));
}

static int owner_command(struct async_carrier *carrier, int operation,
                         int value) {
  int sequence = atomic_load(&control.request) + 1;
  control.carrier = carrier;
  control.operation = operation;
  control.value = value;
  atomic_store_explicit(&control.request, sequence, memory_order_release);
  wake(&control.request);
  int previous;
  while ((previous = atomic_load_explicit(&control.response,
                                          memory_order_acquire)) != sequence) {
    wait_word(&control.response, previous);
  }
  return control.error;
}

static void host_check(void) {
  assert(errno == EDOM && host_tls == 17 && fegetround() == FE_DOWNWARD);
}

static void host_signal(int number, siginfo_t *information, void *context) {
  (void)context;
  assert(number == TEST_LOGICAL_SIGNAL && current && host_tls == 17);
  assert(information->si_code == SI_TKILL);
  atomic_fetch_add_explicit(&current->host_signal_count, 1,
                            memory_order_release);
}

static void nested_host_signal(int number, siginfo_t *information,
                               void *context) {
  assert(number == TEST_NESTED_HOST_SIGNAL && current && host_tls == 17);
  assert(information->si_code == SI_TKILL);
  assert((uintptr_t)__builtin_thread_pointer() == current->exchange.host.tp);
#ifdef GW_TEST_NO_HOST_ANCHOR
  assert(context != current->interrupted_host_frame);
  static const char detected[] =
      "{\"result\":\"PASS\",\"missing_host_anchor_detected\":true}\n";
  assert(syscall(SYS_write, STDOUT_FILENO, detected, sizeof detected - 1) ==
         (ssize_t)(sizeof detected - 1));
  _exit(0); /* Disposable control: no continuation/lifecycle claim. */
#endif
  assert(context == current->interrupted_host_frame);
  assert(signal_host_retirement_busy());
  assert(fegetround() == FE_DOWNWARD);
  ucontext_t *frame = context;
#if defined(__x86_64__)
  assert((uintptr_t)frame->uc_mcontext.gregs[REG_RIP] ==
         current->interrupted_host_pc);
#else
  assert(frame->uc_mcontext.pc == current->interrupted_host_pc);
#endif
  assert(gw_signal_interrupted_sp(frame) == current->interrupted_host_sp);
  char local;
  uintptr_t here = (uintptr_t)&local;
  if (current->host_alternate && current->host_onstack) {
    assert(gw_stack_contains(&frame->uc_stack, here));
  } else {
    assert(here >= current->host_stack_low && here < current->host_stack_high);
  }
  stack_t native_stack;
  assert(!sigaltstack(NULL, &native_stack));
  if (!current->host_alternate ||
      (current->host_onstack && current->host_autodisarm)) {
    assert(native_stack.ss_flags & SS_DISABLE);
  } else {
    assert(!!(native_stack.ss_flags & SS_ONSTACK) == current->host_onstack);
    assert(!!((unsigned)native_stack.ss_flags & GW_SS_AUTODISARM) ==
           current->host_autodisarm);
  }
  sigaddset(&frame->uc_sigmask, SIGWINCH);
  ++current->nested_host_count;
}

void gw_signal_mask_wait_retry_observe(long number,
                                       const struct timespec *remaining) {
  if (current && current->completed_work) {
    assert(number == SYS_clock_nanosleep || number == SYS_ppoll);
    assert(remaining && !remaining->tv_sec && remaining->tv_nsec >= 0 &&
           remaining->tv_nsec <= 350000000);
    ++current->budget_observed;
  }
}

static void notification(int number, siginfo_t *information, void *context) {
  (void)information;
  assert(number == control.notification && current && host_tls == 17);
  ucontext_t *frame = context;
#if defined(__x86_64__)
  uintptr_t pc = frame->uc_mcontext.gregs[REG_RIP];
#else
  uintptr_t pc = frame->uc_mcontext.pc;
#endif
  int in_guest = 0;
  if (atomic_load(&current->self_phase)) {
    in_guest = 1;
    ++current->self_handlers;
  } else if (pc >= current->busy_begin && pc < current->busy_end) {
    in_guest = 1;
    ++current->guest_handlers;
  } else {
    ++current->host_handlers;
  }
  current->interrupted_host_frame = in_guest ? NULL : frame;
  current->interrupted_host_pc = pc;
  current->interrupted_host_sp = gw_signal_interrupted_sp(frame);
  stack_t entry_stack = {.ss_sp = current->entry_stack, .ss_size = 65536};
#ifdef GW_TEST_NO_HOST_ANCHOR
  void *host_frame = NULL;
#else
  void *host_frame = current->interrupted_host_frame;
#endif
  assert(!signal_guest_enter(&current->entry, &entry_stack, host_frame));
  current->interrupted_host_frame = NULL;
  assert(host_tls == 17);
  errno = ERANGE; /* The internal route must preserve interrupted Host errno. */
}

static void *carrier_main(void *argument) {
  struct async_carrier *carrier = argument;
  current = carrier;
  struct exchange *exchange = &carrier->exchange;
  exchange->host_tid = syscall(SYS_gettid);
  exchange->swap = domain_swap;
  exchange->stack_low = (uintptr_t)stack_alloc(1024 * 1024);
  exchange->stack_high = exchange->stack_low + 1024 * 1024;
  carrier->entry_stack = stack_alloc(65536);
  atomic_store_explicit(&carrier->phase, PHASE_READY, memory_order_release);
  while (atomic_load_explicit(&carrier->phase, memory_order_acquire) ==
         PHASE_READY) {
    sched_yield();
  }
  assert(exchange->guest.tp && control.entry);
  exchange->guest.pc = (uintptr_t)domain_start;
#if defined(__x86_64__)
  exchange->guest.saved[2] = (uintptr_t)carrier;
  exchange->guest.saved[3] = (uintptr_t)control.entry;
#else
  exchange->guest.saved[0] = (uintptr_t)carrier;
  exchange->guest.saved[1] = (uintptr_t)control.entry;
#endif
  struct signal_slot *recipient = signal_acquire(exchange);
  assert(recipient);
  pthread_attr_t attributes;
  void *host_stack;
  size_t host_stack_size;
  assert(!pthread_getattr_np(pthread_self(), &attributes));
  assert(!pthread_attr_getstack(&attributes, &host_stack, &host_stack_size));
  assert(!pthread_attr_destroy(&attributes));
  carrier->host_stack_low = (uintptr_t)host_stack;
  carrier->host_stack_high = (uintptr_t)host_stack + host_stack_size;
  if (!carrier->host_alternate) {
    stack_t disabled = {.ss_flags = SS_DISABLE};
    gw_signal_stack_set(&disabled);
  } else if (carrier->host_autodisarm) {
    stack_t alternate = {.ss_sp = recipient->signal_stack,
                         .ss_size = 65536,
                         .ss_flags = GW_SS_AUTODISARM};
    gw_signal_stack_set(&alternate);
  }
  assert(!fesetround(FE_DOWNWARD));
  errno = EDOM;
  uint64_t host_mask = gw_nonlocal_mask_block();
  exchange->swap(&exchange->host, &exchange->guest);
  gw_nonlocal_mask_restore(host_mask);
  host_check();
  assert(exchange->host.tp != exchange->guest.tp);

  sigset_t wake_mask, previous;
  sigemptyset(&wake_mask);
  sigaddset(&wake_mask, control.notification);
  sigaddset(&wake_mask, TEST_LOGICAL_SIGNAL);
  sigaddset(&wake_mask, TEST_NESTED_HOST_SIGNAL);
  assert(!pthread_sigmask(SIG_UNBLOCK, &wake_mask, &previous));
  atomic_store_explicit(&carrier->phase, PHASE_BUSY, memory_order_release);
  while (atomic_load_explicit(&carrier->received, memory_order_acquire) <
         TEST_PACKETS) {
    __asm__ volatile("" ::: "memory");
  }
  host_check();
  sigset_t after_nested;
  assert(!pthread_sigmask(SIG_SETMASK, NULL, &after_nested));
  assert(sigismember(&after_nested, SIGWINCH));

  carrier->delay = 1;
  atomic_store_explicit(&carrier->phase, PHASE_WAIT, memory_order_release);
  struct timespec duration = {.tv_nsec = 500000000},
                  remaining = {.tv_sec = 7193};
  assert(!nanosleep(&duration, &remaining));
  assert(remaining.tv_sec == 7193);
  host_check();
  assert(atomic_load(&carrier->received) == TEST_PACKETS + 1);
  assert(carrier->budget_observed);
  carrier->completed_work = 0;
  carrier->budget_observed = 0;
  atomic_store_explicit(&carrier->phase, PHASE_POLL, memory_order_release);
  assert(!ppoll(NULL, 0, &duration, NULL));
  host_check();
  assert(atomic_load(&carrier->received) == TEST_PACKETS + 2);
  assert(carrier->budget_observed);
  carrier->delay = 0;
  atomic_store_explicit(&carrier->phase, PHASE_HOST_INTERRUPT,
                        memory_order_release);
  struct timespec interrupted_wait = {.tv_sec = 5};
  assert(ppoll(NULL, 0, &interrupted_wait, NULL) == -1 && errno == EINTR);
  errno = EDOM;
  host_check();
  host_mask = gw_nonlocal_mask_block();
  exchange->host_execution_mask = host_mask;
  exchange->host_mask_valid = 1;
  gw_signal_stack_get(&exchange->host_altstack);
  atomic_store_explicit(&carrier->phase, PHASE_GUEST, memory_order_release);
  exchange->swap(&exchange->host, &exchange->guest);
  gw_nonlocal_mask_restore(host_mask);
  host_check();
  assert(atomic_load(&carrier->phase) == PHASE_DONE);
  while (!atomic_load_explicit(&carrier->closed, memory_order_acquire)) {
    sched_yield();
  }
  /* The sender revoked publication while this TID and Guest TCB were still
   * live. Drain only this fixture's helper before retiring any entry storage.
   */
  assert(!pthread_sigmask(SIG_BLOCK, &wake_mask, NULL));
  uint64_t bit = UINT64_C(1) << (control.notification - 1);
  struct timespec zero = {0};
  while (syscall(SYS_rt_sigtimedwait, &bit, NULL, &zero, sizeof bit) >= 0) {
  }
  assert(errno == EAGAIN);
  errno = EDOM;
  host_mask = gw_nonlocal_mask_block();
  exchange->swap(&exchange->host, &exchange->guest);
  gw_nonlocal_mask_restore(host_mask);
  host_check();
  assert(!signal_prepare_release(recipient, 0));
  signal_release(recipient);
  assert(!pthread_sigmask(SIG_SETMASK, &previous, NULL));
  return carrier;
}

static void wait_value(_Atomic int *word, int value) {
  struct timespec start, now;
  assert(!clock_gettime(CLOCK_MONOTONIC, &start));
  while (atomic_load_explicit(word, memory_order_acquire) != value) {
    assert(!clock_gettime(CLOCK_MONOTONIC, &now));
    assert(now.tv_sec - start.tv_sec < 5);
    sched_yield();
  }
}

static void wait_native_sleep(struct async_carrier *carrier, int phase) {
  wait_value(&carrier->phase, phase);
  struct timespec start, now;
  assert(!clock_gettime(CLOCK_MONOTONIC, &start));
  for (;;) {
    char path[96], text[256];
    snprintf(path, sizeof path, "/proc/self/task/%ld/syscall",
             carrier->exchange.host_tid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    assert(fd >= 0);
    ssize_t length = read(fd, text, sizeof text - 1);
    assert(!close(fd));
    if (length > 0) {
      text[length] = 0;
      long number;
      if (sscanf(text, "%ld", &number) == 1 &&
          number == (phase == PHASE_WAIT ? SYS_clock_nanosleep : SYS_ppoll)) {
        return;
      }
    }
    assert(!clock_gettime(CLOCK_MONOTONIC, &now));
    assert(now.tv_sec - start.tv_sec < 5);
    sched_yield();
  }
}

static void start_owner(char **argv) {
  char pointer[32];
  snprintf(pointer, sizeof pointer, "%lx", (unsigned long)(uintptr_t)&control);
  char *arguments[] = {argv[1], "--library-path", argv[2], argv[3], pointer};
  struct gw_bootstrap_image image = gw_bootstrap_map(argv[1]);
  void *stack = stack_alloc(1024 * 1024);
  void *sp = gw_bootstrap_stack(image, stack, 1024 * 1024, arguments, 5);
  assert(launch_guest(CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
                          CLONE_THREAD | CLONE_SYSVSEM,
                      sp, image.entry) > 0);
  wait_value(&control.ready, 1);
}

static void prepare_routes(void) {
  assert(control.notification != TEST_LOGICAL_SIGNAL);
  assert(TEST_LOGICAL_SIGNAL >= SIGRTMIN && TEST_LOGICAL_SIGNAL <= SIGRTMAX);
  assert(!signal_router_init());
  assert(!signal_router_adopt());
  int lanes[GW_SIGNAL_FAULT_LANES];
  for (int i = 0; i < GW_SIGNAL_FAULT_LANES; ++i) {
    lanes[i] = SIGRTMIN + i;
  }
  assert(!signal_router_defer_faults(lanes));
  int internal_error =
      signal_router_internal_action(control.notification, notification);
  if (internal_error) {
    fprintf(stderr,
            "INTERNAL_ROUTE notification=%d rtmin=%d rtmax=%d error=%d\n",
            control.notification, SIGRTMIN, SIGRTMAX, internal_error);
  }
  assert(!internal_error);
  assert(signal_router_internal_action(control.notification, notification) ==
         EBUSY);
  struct sigaction action = {.sa_handler = SIG_IGN};
  assert(sigaction(control.notification, &action, NULL) == -1 &&
         errno == EBUSY);
  assert(sigaction(control.notification, NULL, &action) == -1 &&
         errno == EBUSY);
  assert(!signal_router_guest_ready());
  struct sigaction own_action = {.sa_sigaction = host_signal,
                                 .sa_flags = SA_SIGINFO};
  assert(!sigaction(TEST_LOGICAL_SIGNAL, &own_action, NULL));
  assert(signal_router_internal_action(TEST_LOGICAL_SIGNAL, notification) ==
         EBUSY);
  assert(signal_router_internal_action(SIGKILL, notification) == EINVAL);
}

static void start_carriers(int batch, pthread_t threads[TEST_CARRIERS]) {
  struct sigaction action = {
      .sa_sigaction = nested_host_signal,
      .sa_flags = SA_SIGINFO | (batch & 1 ? SA_ONSTACK : 0)};
  sigemptyset(&action.sa_mask);
  assert(!sigaction(TEST_NESTED_HOST_SIGNAL, &action, NULL));
  for (int i = 0; i < TEST_CARRIERS; ++i) {
    carriers[i] = (struct async_carrier){.index = batch * TEST_CARRIERS + i,
                                         .host_alternate = batch >= 2,
                                         .host_autodisarm = batch >= 4,
                                         .host_onstack = batch & 1};
    assert(!pthread_create(&threads[i], NULL, carrier_main, &carriers[i]));
    wait_value(&carriers[i].phase, PHASE_READY);
    assert(!owner_command(&carriers[i], OWNER_ALLOC, 0));
    atomic_store_explicit(&carriers[i].phase, 0, memory_order_release);
  }
}

static void send_busy_packets(void) {
  for (int i = 0; i < TEST_CARRIERS; ++i) {
    wait_value(&carriers[i].phase, PHASE_BUSY);
    assert(!syscall(SYS_tgkill, getpid(), carriers[i].exchange.host_tid,
                    TEST_LOGICAL_SIGNAL));
    wait_value(&carriers[i].host_signal_count, 1);
    for (int packet = 1; packet <= TEST_PACKETS; ++packet) {
      assert(!owner_command(&carriers[i], OWNER_SEND,
                            carriers[i].index * 1000 + packet));
      wait_value(&carriers[i].received, packet);
    }
  }
}

static void send_wait_packets(void) {
  for (int polling = 0; polling < 2; ++polling) {
    for (int i = 0; i < TEST_CARRIERS; ++i) {
      int phase = polling ? PHASE_POLL : PHASE_WAIT;
      wait_native_sleep(&carriers[i], phase);
      if (polling) {
        int empty = atomic_load(&carriers[i].empty_notifications);
        assert(!owner_command(&carriers[i], OWNER_EMPTY, 0));
        wait_value(&carriers[i].empty_notifications, empty + 1);
        wait_native_sleep(&carriers[i], phase);
      }
      assert(!owner_command(
          &carriers[i], OWNER_SEND,
          carriers[i].index * 1000 + TEST_PACKETS + 1 + polling));
      wait_value(&carriers[i].received, TEST_PACKETS + 1 + polling);
    }
  }
  for (int i = 0; i < TEST_CARRIERS; ++i) {
    wait_native_sleep(&carriers[i], PHASE_HOST_INTERRUPT);
    assert(!owner_command(&carriers[i], OWNER_EMPTY, 0));
  }
}

static void send_guest_packets_and_close(void) {
  for (int i = 0; i < TEST_CARRIERS; ++i) {
    atomic_store_explicit(&carriers[i].self_allowed, 1, memory_order_release);
    wake(&carriers[i].self_allowed);
    wait_value(&carriers[i].guest_running, 1);
    assert(carriers[i].self_received == 6);
#ifdef __ANDROID__
    if (!i) {
      private_host_routes();
    }
#endif
    assert(!owner_command(&carriers[i], OWNER_SEND,
                          carriers[i].index * 1000 + TEST_TOTAL_PACKETS));
    wait_value(&carriers[i].phase, PHASE_DONE);
    assert(!owner_command(&carriers[i], OWNER_CLOSE, 0));
    assert(owner_command(&carriers[i], OWNER_SEND, 9913) == ESRCH);
  }
}

struct totals {
  int packets, self_packets, host_entries, guest_entries, self_entries;
};

static void join_carriers(pthread_t threads[TEST_CARRIERS],
                          struct totals *sum) {
  for (int i = 0; i < TEST_CARRIERS; ++i) {
    void *result;
    assert(!pthread_join(threads[i], &result) && result == &carriers[i]);
    assert(carriers[i].guest_handlers == 1 && carriers[i].host_handlers >= 2);
    assert(carriers[i].self_handlers == 6);
    assert(carriers[i].nested_host_count == TEST_PACKETS + 1);
    assert(atomic_load(&carriers[i].host_signal_count) == 1);
    assert(!owner_command(&carriers[i], OWNER_FREE, 0));
    sum->packets += atomic_load(&carriers[i].received);
    sum->self_packets += carriers[i].self_received;
    sum->host_entries += carriers[i].host_handlers;
    sum->guest_entries += carriers[i].guest_handlers;
    sum->self_entries += carriers[i].self_handlers;
    stack_free((void *)carriers[i].exchange.stack_low, 1024 * 1024);
    stack_free(carriers[i].entry_stack, 65536);
  }
}

int main(int argc, char **argv) {
  assert(argc == 5);
  control.notification = atoi(argv[4]);
  test_layout(control.layout);
  prepare_routes();
  assert(!signal_router_lease_guest_nptl());
  struct kernel_action before[9], after[9];
  snapshot_private(before);
  start_owner(argv);
  snapshot_private(after);
  assert(!memcmp(before, after, sizeof before));
  for (int number = 63; number <= 64; ++number) {
    struct sigaction action;
    errno = 0;
    assert(sigaction(number, NULL, &action) == -1 && errno == EBUSY);
    struct kernel_action native = {0};
    assert(!syscall(SYS_rt_sigaction, number, NULL, &native, sizeof(uint64_t)));
    assert(native.handler > 1);
  }
  assert(control.send_lifecycle);

  struct totals sum = {0};
  for (int batch = 0; batch < TEST_BATCHES; ++batch) {
    pthread_t threads[TEST_CARRIERS];
    start_carriers(batch, threads);
    send_busy_packets();
    send_wait_packets();
    send_guest_packets_and_close();
    join_carriers(threads, &sum);
  }
  assert(!signal_router_verify());
  assert(!signal_host_retirement_busy());
  assert(!signal_router_restore());
  printf(
      "{\"result\":\"PASS\",\"packets\":%d,\"self_packets\":%d,"
      "\"host_entries\":%d,"
      "\"guest_entries\":%d,\"self_entries\":%d,"
      "\"same_tid\":true,\"distinct_tls\":true,"
      "\"guest_stack\":true,\"tls_errno_fp\":true,\"guest_malloc\":true,"
      "\"layout_handshake\":true,"
      "\"busy_host_delivery\":true,\"host_sleep_transparent\":true,"
      "\"host_ppoll_transparent\":true,\"handler_time_in_budget\":true,"
      "\"empty_notification_retry\":true,"
      "\"host_disposition_preserved\":true,"
      "\"nested_host_original_context\":true,\"nested_host_stacks\":true,"
      "\"nested_host_wait_interrupt\":true,"
      "\"nested_host_autodisarm\":true,"
      "\"public_send_hooks\":true,\"send_exit_serialization\":true,"
      "\"self_sends_before_return\":true,\"send_errno_contract\":true,"
      "\"handler_errno_visible\":true,"
      "\"runtime_target_registry\":true,\"entry_pins_balanced\":true,"
      "\"nptl_signal_isolation\":true,\"guest_cancel_timer\":true,"
#ifdef __ANDROID__
      "\"bionic_private_routes\":true,"
#else
      "\"bionic_private_routes\":false,"
#endif
      "\"recipient_close\":true,\"production_ready\":false}\n",
      sum.packets, sum.self_packets, sum.host_entries, sum.guest_entries,
      sum.self_entries);
  fflush(stdout);
  _exit(0);
}
