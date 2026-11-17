#define _GNU_SOURCE
#include "signal-inbox.h"
#include "signal_contract.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <poll.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static uint64_t bit(int number) {
  return UINT64_C(1) << (number - 1);
}

static void block(void) {
  uint64_t all = UINT64_MAX;
  assert(!syscall(SYS_rt_sigprocmask, SIG_BLOCK, &all, NULL, sizeof all));
}

static siginfo_t packet(int number, int value) {
  siginfo_t info = {.si_signo = number, .si_code = SI_QUEUE};
  info.si_pid = getpid();
  info.si_uid = getuid();
  info.si_value.sival_int = value;
  return info;
}

static void put(struct gw_signal_inbox *inbox,
                struct gw_signal_receiver *receiver, int number, int value) {
  siginfo_t info = packet(number, value);
  struct gw_signal_publication result =
      gw_signal_inbox_put(inbox, receiver, &info, NULL, NULL);
  assert(!result.status && !result.wake_error);
}

static int take(struct gw_signal_receiver *receiver, uint64_t eligible,
                int value, enum gw_signal_queue_source source) {
  siginfo_t observed;
  enum gw_signal_queue_source actual;
  int number = gw_signal_inbox_take(receiver, eligible, &observed, &actual);
  assert(number && actual == source && observed.si_signo == number);
  assert(observed.si_code == SI_QUEUE && observed.si_pid == getpid() &&
         observed.si_uid == getuid() && observed.si_value.sival_int == value);
  return number;
}

static void close_all(struct gw_signal_inbox *inbox,
                      struct gw_signal_receiver *receiver) {
  gw_signal_receiver_close(receiver);
  gw_signal_inbox_close(inbox);
  assert(!inbox->queued && !gw_signal_receiver_destroy(receiver));
  assert(!gw_signal_inbox_destroy(inbox));
}

/* Real kernel queues are the oracle, including asynchronous sends whose
 * number happens to name a synchronous fault. Never invoke a fault handler.
 * The child exits with unselected blocked signals still pending. */
static void ordering(int variant) {
  block();
  struct gw_signal_inbox inbox;
  struct gw_signal_receiver receiver = {0};
  gw_signal_inbox_init(&inbox, 0, 1024);
  assert(!gw_signal_receiver_open(&inbox, &receiver, UINT64_MAX));
  int numbers[] = {SIGHUP, SIGSEGV, SIGUSR1, SIGSYS,       SIGILL,
                   SIGBUS, SIGFPE,  SIGTRAP, SIGRTMIN + 7, SIGRTMIN + 8};
  uint64_t eligible = UINT64_MAX;
  if (variant % 3 == 1) {
    eligible &= ~(bit(SIGILL) | bit(SIGTRAP) | bit(SIGRTMIN + 7));
  } else if (variant % 3 == 2) {
    eligible = bit(SIGUSR1) | bit(SIGSEGV) | bit(SIGRTMIN + 8);
  }

  for (int i = 0; i < 80; ++i) {
    int number = numbers[(i * 7 + variant) % 10];
    int shared = (i / 10 + variant) % 2;
    int value = i + 1000 * shared;
    siginfo_t info = packet(number, value);
    assert(!(shared ? syscall(SYS_rt_sigqueueinfo, getpid(), number, &info)
                    : syscall(SYS_rt_tgsigqueueinfo, getpid(),
                              syscall(SYS_gettid), number, &info)));
    put(&inbox, shared ? NULL : &receiver, number, value);
  }

  struct timespec zero = {0};
  for (;;) {
    siginfo_t native, observed;
    enum gw_signal_queue_source source;
    int actual = syscall(SYS_rt_sigtimedwait, &eligible, &native, &zero, 8);
    int selected =
        gw_signal_inbox_take(&receiver, eligible, &observed, &source);
    if (actual < 0) {
      assert(errno == EAGAIN && !selected && source == GW_SIGNAL_NONE);
      break;
    }
    assert(selected == actual && observed.si_signo == native.si_signo &&
           observed.si_code == native.si_code &&
           observed.si_pid == native.si_pid &&
           observed.si_uid == native.si_uid &&
           observed.si_value.sival_int == native.si_value.sival_int);
    assert(source == (native.si_value.sival_int >= 1000 ? GW_SIGNAL_PROCESS
                                                        : GW_SIGNAL_THREAD));
  }

  close_all(&inbox, &receiver);
}

static void resources(void) {
  struct gw_signal_inbox inbox, other;
  struct gw_signal_receiver a = {0}, b = {0};
  gw_signal_inbox_init(&inbox, bit(SIGUSR2), 2);
  gw_signal_inbox_init(&other, 0, 2);
  assert(!gw_signal_receiver_open(&inbox, &a, bit(SIGUSR1)));
  assert(!gw_signal_receiver_open(&inbox, &b, 0));
  assert(gw_signal_receiver_open(&inbox, &a, 0) == EALREADY);
  assert(gw_signal_inbox_destroy(&inbox) == EBUSY);
  assert(gw_signal_receiver_destroy(&a) == EBUSY);

  siginfo_t info = packet(SIGUSR2, 0);
  assert(gw_signal_inbox_put(&inbox, &a, &info, NULL, NULL).status == EBUSY);
  info = packet(SIGKILL, 0);
  assert(gw_signal_inbox_put(&inbox, NULL, &info, NULL, NULL).status == EINVAL);
  info = packet(SIGUSR1, 0);
  assert(gw_signal_inbox_put(&other, &a, &info, NULL, NULL).status == EINVAL);
  info.si_code = SEGV_MAPERR;
  assert(gw_signal_inbox_put(&inbox, &a, &info, NULL, NULL).status == EINVAL);
  assert(!gw_signal_inbox_pending(&a));

  put(&inbox, NULL, SIGRTMIN + 7, 1);
  put(&inbox, &a, SIGRTMIN + 7, 2);
  info = packet(SIGRTMIN + 8, 3);
  assert(gw_signal_inbox_put(&inbox, &b, &info, NULL, NULL).status == EAGAIN);
  assert(inbox.queued == 2);
  put(&inbox, &a, SIGUSR1, 4);
  put(&inbox, &a, SIGUSR1, 5);
  put(&inbox, NULL, SIGUSR1, 6);
  assert(inbox.queued == 2);
  assert(gw_signal_inbox_pending(&a) == (bit(SIGUSR1) | bit(SIGRTMIN + 7)));
  assert(gw_signal_inbox_pending(&b) == (bit(SIGUSR1) | bit(SIGRTMIN + 7)));
  assert(take(&a, UINT64_MAX, 4, GW_SIGNAL_THREAD) == SIGUSR1);
  assert(take(&a, UINT64_MAX, 2, GW_SIGNAL_THREAD) == SIGRTMIN + 7);
  assert(take(&b, UINT64_MAX, 6, GW_SIGNAL_PROCESS) == SIGUSR1);
  assert(take(&b, UINT64_MAX, 1, GW_SIGNAL_PROCESS) == SIGRTMIN + 7);
  assert(!inbox.queued && !gw_signal_inbox_pending(&a));

  put(&inbox, &a, SIGRTMIN + 7, 7);
  gw_signal_receiver_close(&a);
  gw_signal_receiver_close(&a);
  assert(!inbox.queued && inbox.receivers == 1);
  assert(gw_signal_inbox_put(&inbox, &a, &info, NULL, NULL).status == ESRCH);
  assert(!gw_signal_receiver_destroy(&a));
  assert(!gw_signal_receiver_open(&inbox, &a, 0));
  put(&inbox, &a, SIGRTMIN + 7, 8);
  put(&inbox, NULL, SIGRTMIN + 7, 9);
  gw_signal_inbox_close(&inbox);
  assert(inbox.queued == 1 && !gw_signal_inbox_pending(&a));
  assert(gw_signal_inbox_destroy(&inbox) == EBUSY);
  gw_signal_receiver_close(&b);
  assert(!gw_signal_receiver_destroy(&b));
  close_all(&inbox, &a);
  gw_signal_inbox_close(&other);
  assert(!gw_signal_inbox_destroy(&other));
}

enum { PRODUCERS = 4, MESSAGES = 600 };
static struct gw_signal_inbox concurrent;
static struct gw_signal_receiver consumers[2];
static _Atomic unsigned seen[PRODUCERS * MESSAGES], received, producers_done;

static void *produce(void *argument) {
  block();
  int id = (int)(intptr_t)argument;
  for (int i = 0; i < MESSAGES; ++i) {
    int value = id * MESSAGES + i;
    put(&concurrent, i % 3 ? NULL : &consumers[id % 2], SIGRTMIN + 7, value);
  }
  atomic_fetch_add(&producers_done, 1);
  return NULL;
}

static void *consume(void *argument) {
  block();
  struct gw_signal_receiver *receiver = argument;
  for (;;) {
    siginfo_t info;
    enum gw_signal_queue_source source;
    int number = gw_signal_inbox_take(receiver, UINT64_MAX, &info, &source);
    if (number) {
      int value = info.si_value.sival_int;
      assert(number == SIGRTMIN + 7 && value >= 0 &&
             value < PRODUCERS * MESSAGES &&
             !atomic_fetch_add(&seen[value], 1));
      assert(source ==
             (value % MESSAGES % 3 ? GW_SIGNAL_PROCESS : GW_SIGNAL_THREAD));
      atomic_fetch_add(&received, 1);
    } else if (atomic_load(&producers_done) == PRODUCERS &&
               atomic_load(&received) == PRODUCERS * MESSAGES) {
      break;
    } else {
      sched_yield();
    }
  }
  return NULL;
}

static void contention(void) {
  gw_signal_inbox_init(&concurrent, 0, PRODUCERS * MESSAGES);
  pthread_t writers[PRODUCERS], readers[2];
  for (int i = 0; i < 2; ++i) {
    assert(!gw_signal_receiver_open(&concurrent, &consumers[i], 0));
    assert(!pthread_create(&readers[i], NULL, consume, &consumers[i]));
  }
  for (intptr_t i = 0; i < PRODUCERS; ++i) {
    assert(!pthread_create(&writers[i], NULL, produce, (void *)i));
  }
  for (int i = 0; i < PRODUCERS; ++i) {
    assert(!pthread_join(writers[i], NULL));
  }
  for (int i = 0; i < 2; ++i) {
    assert(!pthread_join(readers[i], NULL));
    gw_signal_receiver_close(&consumers[i]);
    assert(!gw_signal_receiver_destroy(&consumers[i]));
  }
  assert(!concurrent.queued);
  gw_signal_inbox_close(&concurrent);
  assert(!gw_signal_inbox_destroy(&concurrent));
}

static struct gw_signal_inbox active;
static struct gw_signal_receiver busy;
static _Atomic int ready, delivered, host_delivered, release_busy;
static _Atomic int awakenings;
static _Atomic long target_tid;
static _Thread_local int recipient_thread;

static void notify_handler(int number) {
  assert(number == SIGRTMIN + 9 && recipient_thread);
  int saved = errno;
  siginfo_t info;
  enum gw_signal_queue_source source;
  while (gw_signal_inbox_deliver(&busy, &info, &source)) {
    assert(info.si_signo == SIGUSR1 && source == GW_SIGNAL_THREAD &&
           info.si_value.sival_int == 71);
    atomic_fetch_add(&delivered, 1);
  }
  atomic_fetch_add(&awakenings, 1);
  errno = saved;
}

static void host_handler(int number) {
  assert(number == SIGUSR1 && recipient_thread);
  atomic_fetch_add(&host_delivered, 1);
}

static int notify(void *argument, struct gw_signal_receiver *receiver) {
  assert(receiver == &busy);
  if (argument) {
    return EAGAIN;
  }

  return syscall(SYS_tgkill, getpid(), atomic_load(&target_tid), SIGRTMIN + 9)
             ? errno
             : 0;
}

static void *busy_host(void *unused) {
  (void)unused;
  block();
  recipient_thread = 1;
  atomic_store(&target_tid, syscall(SYS_gettid));
  sigset_t unblocked;
  sigemptyset(&unblocked);
  sigaddset(&unblocked, SIGRTMIN + 9);
  sigaddset(&unblocked, SIGUSR1);
  assert(!pthread_sigmask(SIG_UNBLOCK, &unblocked, NULL));
  atomic_store_explicit(&ready, 1, memory_order_release);

  /* No Guest entry or queue polling in this loop. A real kernel notification
   * must interrupt this Host computation and run the receiver on this thread.
   * This tests the inbox/wakeup contract, not foreign TLS or a live VM. */
  while (!atomic_load_explicit(&release_busy, memory_order_acquire)) {
    atomic_signal_fence(memory_order_seq_cst);
  }
  block();
  return NULL;
}

static void wait_for(_Atomic int *value, int wanted) {
  struct timespec start, now;
  assert(!clock_gettime(CLOCK_MONOTONIC, &start));
  while (atomic_load_explicit(value, memory_order_acquire) != wanted) {
    assert(!clock_gettime(CLOCK_MONOTONIC, &now));
    assert(now.tv_sec - start.tv_sec < 5);
    sched_yield();
  }
}

static void timely(void) {
  struct sigaction action = {.sa_handler = notify_handler};
  sigfillset(&action.sa_mask);
  assert(!sigaction(SIGRTMIN + 9, &action, NULL));
  action.sa_handler = host_handler;
  assert(!sigaction(SIGUSR1, &action, NULL));
  gw_signal_inbox_init(&active, 0, 100);
  assert(!gw_signal_receiver_open(&active, &busy, bit(SIGUSR1)));
  pthread_t thread;
  assert(!pthread_create(&thread, NULL, busy_host, NULL));
  wait_for(&ready, 1);

  siginfo_t info = packet(SIGUSR1, 71);
  struct gw_signal_publication result =
      gw_signal_inbox_put(&active, &busy, &info, notify, NULL);
  assert(!result.status && !result.wake_error && !atomic_load(&delivered));
  assert(gw_signal_inbox_pending(&busy) == bit(SIGUSR1));
  /* A previously armed notification may arrive after mask/selection changed.
   * It must not consume a still-blocked logical packet. */
  assert(!notify(NULL, &busy));
  wait_for(&awakenings, 1);
  assert(!atomic_load(&delivered) &&
         gw_signal_inbox_pending(&busy) == bit(SIGUSR1));
  assert(!gw_signal_receiver_mask(&busy, 0, notify, NULL));
  wait_for(&delivered, 1);

  /* Failed notification must remain distinguishable from rejected send.
   * Retrying the wake must neither lose nor duplicate the accepted message. */
  result = gw_signal_inbox_put(&active, &busy, &info, notify, &active);
  assert(!result.status && result.wake_error == EAGAIN);
  assert(gw_signal_inbox_pending(&busy) == bit(SIGUSR1));
  assert(!gw_signal_receiver_mask(&busy, 0, notify, NULL));
  wait_for(&delivered, 2);
  assert(!atomic_load(&host_delivered));
  assert(!syscall(SYS_tgkill, getpid(), atomic_load(&target_tid), SIGUSR1));
  wait_for(&host_delivered, 1);
  assert(atomic_load(&delivered) == 2);

  atomic_store_explicit(&release_busy, 1, memory_order_release);
  assert(!pthread_join(thread, NULL));
  close_all(&active, &busy);
}

static struct gw_signal_inbox closing;
static struct gw_signal_receiver closing_receiver;
static _Atomic int wake_entered, permit_wake, close_entered, closed;
static _Atomic int notified;

static int held_notification(void *unused,
                             struct gw_signal_receiver *receiver) {
  (void)unused;
  assert(receiver == &closing_receiver && !receiver->closed);
  atomic_store_explicit(&wake_entered, 1, memory_order_release);
  /* Test-only suspension at the native-notification boundary. A production
   * callback must be nonblocking. Close must not revoke target lifetime while
   * this send still owns the inbox transaction. */
  while (!atomic_load_explicit(&permit_wake, memory_order_acquire)) {
    atomic_signal_fence(memory_order_seq_cst);
  }
  assert(!atomic_load(&closed) && !receiver->closed);
  atomic_fetch_add(&notified, 1);
  return 0;
}

static void *closing_sender(void *unused) {
  (void)unused;
  block();
  siginfo_t info = packet(SIGRTMIN + 7, 71);
  struct gw_signal_publication result = gw_signal_inbox_put(
      &closing, &closing_receiver, &info, held_notification, NULL);
  assert(!result.status && !result.wake_error);
  return NULL;
}

static void *close_receiver(void *unused) {
  (void)unused;
  block();
  atomic_store_explicit(&close_entered, 1, memory_order_release);
  gw_signal_receiver_close(&closing_receiver);
  atomic_store_explicit(&closed, 1, memory_order_release);
  return NULL;
}

static void close_window(void) {
  gw_signal_inbox_init(&closing, 0, 8);
  assert(!gw_signal_receiver_open(&closing, &closing_receiver, 0));
  pthread_t sender, closer;
  assert(!pthread_create(&sender, NULL, closing_sender, NULL));
  wait_for(&wake_entered, 1);
  assert(!pthread_create(&closer, NULL, close_receiver, NULL));
  wait_for(&close_entered, 1);
  assert(!atomic_load(&closed));
  atomic_store_explicit(&permit_wake, 1, memory_order_release);
  assert(!pthread_join(sender, NULL) && !pthread_join(closer, NULL));
  assert(atomic_load(&notified) == 1 && !closing.queued);

  siginfo_t info = packet(SIGRTMIN + 7, 72);
  struct gw_signal_publication result = gw_signal_inbox_put(
      &closing, &closing_receiver, &info, held_notification, NULL);
  assert(result.status == ESRCH && !result.wake_error &&
         atomic_load(&notified) == 1);
  assert(gw_signal_receiver_mask(&closing_receiver, 0, held_notification,
                                 NULL) == ESRCH);
  close_all(&closing, &closing_receiver);
}

#ifndef __ANDROID__
static struct gw_signal_inbox cancelled;
static struct gw_signal_receiver cancelled_receiver;
static _Atomic int cancel_ready, cancel_proceed, committed;

static void *cancelled_sender(void *unused) {
  (void)unused;
  assert(!pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL));
  /* Sanitizer pthread startup may retain the parent's raw private mask.
   * Establish this test's native cancellation baseline explicitly before
   * the main thread is allowed to request cancellation. */
  uint64_t cancel = bit(32);
  assert(
      !syscall(SYS_rt_sigprocmask, SIG_UNBLOCK, &cancel, NULL, sizeof cancel));
  uint64_t all = UINT64_MAX, previous;
  assert(!syscall(SYS_rt_sigprocmask, SIG_BLOCK, &all, &previous, sizeof all));
  assert(!(previous & bit(32)));
  atomic_store_explicit(&cancel_ready, 1, memory_order_release);
  while (!atomic_load_explicit(&cancel_proceed, memory_order_acquire)) {
    atomic_signal_fence(memory_order_seq_cst);
  }

  put(&cancelled, &cancelled_receiver, SIGRTMIN + 7, 91);
  atomic_store_explicit(&committed, 1, memory_order_release);
  assert(!syscall(SYS_rt_sigprocmask, SIG_SETMASK, &previous, NULL,
                  sizeof previous));
  pthread_testcancel();
  _exit(126);
}

static void cancellation(void) {
  gw_signal_inbox_init(&cancelled, 0, 8);
  assert(!gw_signal_receiver_open(&cancelled, &cancelled_receiver, 0));
  pthread_t sender;
  assert(!pthread_create(&sender, NULL, cancelled_sender, NULL));
  wait_for(&cancel_ready, 1);
  assert(!pthread_cancel(sender));
  atomic_store_explicit(&cancel_proceed, 1, memory_order_release);
  void *result;
  assert(!pthread_join(sender, &result) && result == PTHREAD_CANCELED);
  assert(atomic_load(&committed) == 1);
  assert(take(&cancelled_receiver, UINT64_MAX, 91, GW_SIGNAL_THREAD) ==
         SIGRTMIN + 7);
  close_all(&cancelled, &cancelled_receiver);
}
#endif

static void child_case(void (*run)(void)) {
  pid_t child = fork();
  assert(child >= 0);
  if (!child) {
    block();
    run();
    _exit(0);
  }

  int status;
  assert(waitpid(child, &status, 0) == child);
  assert(WIFEXITED(status) && !WEXITSTATUS(status));
}

static volatile sig_atomic_t empty_notifications;

static void empty_notification(int number) {
  assert(number == SIGRTMIN + 9);
  siginfo_t info;
  enum gw_signal_queue_source source;
  assert(!gw_signal_inbox_deliver(&busy, &info, &source) &&
         source == GW_SIGNAL_NONE);
  ++empty_notifications;
}

static void notification_control(void) {
  block();
  gw_signal_inbox_init(&active, 0, 8);
  assert(!gw_signal_receiver_open(&active, &busy, UINT64_MAX));
  struct sigaction action = {.sa_handler = empty_notification,
                             .sa_flags = SA_RESTART};
  sigfillset(&action.sa_mask);
  assert(!sigaction(SIGRTMIN + 9, &action, NULL));

  /* A stale private notification has no logical Guest handler to execute.
   * Queue it while blocked, then let ppoll atomically admit it. SA_RESTART
   * alone cannot hide this notification from the interrupted Host wait.
   * https://github.com/torvalds/linux/blob/v6.12/fs/select.c
   * See do_sys_poll()/do_restart_poll() and SYSCALL_DEFINE5(ppoll). */
  assert(!syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), SIGRTMIN + 9));
  uint64_t temporary = UINT64_MAX & ~bit(SIGRTMIN + 9);
  struct timespec timeout = {.tv_sec = 1};
  errno = 0;
  assert(syscall(SYS_ppoll, NULL, 0, &timeout, &temporary, 8) == -1 &&
         errno == EINTR && empty_notifications == 1);
  assert(!gw_signal_inbox_pending(&busy));
  close_all(&active, &busy);
  puts(
      "{\"result\":\"CONTROL_ONLY\",\"native_only\":true,"
      "\"sa_restart\":true,\"guest_packet\":false,\"host_ppoll_eintr\":true}");
}

int main(int argc, char **argv) {
  if (argc == 2 && !strcmp(argv[1], "--notification-control")) {
    notification_control();
    return 0;
  }
  assert(argc == 1);
  block();
  for (int variant = 0; variant < 12; ++variant) {
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
      ordering(variant);
      _exit(0);
    }
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && !WEXITSTATUS(status));
  }
  child_case(resources);
  child_case(contention);
  child_case(timely);
  child_case(close_window);
#ifndef __ANDROID__
  child_case(cancellation);
#endif
  puts(
      "{\"result\":\"PASS\",\"native_queue_order\":true,"
      "\"shared_quota\":true,\"recipient_close\":true,"
      "\"notify_close_transaction\":true,"
      "\"concurrent_consumers\":true,\"timely_notification\":true,"
      "\"stale_notification_mask\":true,"
#ifndef __ANDROID__
      "\"native_cancel_guard\":true,"
#else
      "\"pthread_cancel_unavailable\":true,"
#endif
      "\"host_disposition_preserved\":true,\"failed_wake_retained\":true}");
}
