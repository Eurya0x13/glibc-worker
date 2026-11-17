#define _GNU_SOURCE
#include "signal-actions.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

static void handler_a(int number) {
  (void)number;
}

static void handler_b(int number) {
  (void)number;
}

static struct sigaction handler(void (*entry)(int), int flags) {
  struct sigaction action = {.sa_handler = entry, .sa_flags = flags};
  sigemptyset(&action.sa_mask);
  sigaddset(&action.sa_mask, SIGWINCH);
  return action;
}

static void snapshots_and_aliases(void) {
  struct gw_signal_actions actions;
  gw_signal_actions_init(&actions, NULL, UINT64_C(1) << (SIGUSR2 - 1));
  struct sigaction action = handler(handler_a, SA_RESETHAND | SA_NODEFER);
  assert(!gw_signal_actions_change(&actions, SIGUSR1, &action, NULL));
  struct gw_signal_action selected;
  assert(!gw_signal_actions_select(&actions, SIGUSR1, &selected));
  assert(selected.action.sa_handler == handler_a && selected.revision == 1);
  struct sigaction observed;
  assert(!gw_signal_actions_change(&actions, SIGUSR1, NULL, &observed));
  assert(observed.sa_handler == SIG_DFL &&
         observed.sa_flags == action.sa_flags &&
         sigismember(&observed.sa_mask, SIGWINCH));

  action = handler(handler_b, SA_RESTART);
  assert(!gw_signal_actions_change(&actions, SIGUSR1, &action, &action));
  assert(action.sa_handler == SIG_DFL && (action.sa_flags & SA_RESETHAND));
  assert(!gw_signal_actions_change(&actions, SIGUSR1, NULL, &observed));
  assert(observed.sa_handler == handler_b && observed.sa_flags == SA_RESTART);
  /* A previously selected action never refers into the registration table. */
  assert(selected.action.sa_handler == handler_a &&
         (selected.action.sa_flags & SA_RESETHAND));
  assert(gw_signal_actions_change(&actions, SIGUSR2, &action, NULL) == EBUSY);
  for (int number = 0; number <= 65; ++number) {
    if (number == 0 || number == 65 || number == 32 || number == 33 ||
        number == SIGKILL || number == SIGSTOP) {
      assert(gw_signal_actions_change(&actions, number, NULL, NULL) == EINVAL);
    }
  }

  actions.revision = UINT64_MAX;
  action = handler(handler_a, SA_NODEFER);
  assert(gw_signal_actions_change(&actions, SIGUSR1, &action, NULL) ==
         EOVERFLOW);
  assert(!gw_signal_actions_change(&actions, SIGUSR1, NULL, &observed));
  assert(observed.sa_handler == handler_b);
  gw_signal_actions_close(&actions);
  assert(gw_signal_actions_select(&actions, SIGUSR1, &selected) == ESRCH);
}

static struct gw_signal_publication send(struct gw_signal_actions *actions,
                                         struct gw_signal_target *target,
                                         int number, int value) {
  siginfo_t information = {.si_signo = number, .si_code = SI_QUEUE};
  information.si_value.sival_int = value;
  return gw_signal_actions_send(actions, target ? target->key : 0,
                                target ? target->tid : 0, &information);
}

static void ignore_and_quota(void) {
  struct gw_signal_targets targets;
  struct gw_signal_actions actions;
  gw_signal_targets_init(&targets, UINT64_C(1) << (SIGUSR2 - 1), 4, NULL, NULL);
  gw_signal_actions_init(&actions, &targets, 0);
  struct gw_signal_target *a, *b;
  assert(!gw_signal_target_open(&targets, 1, 1, 0, NULL, &a));
  assert(!gw_signal_target_open(&targets, 2, 2, 0, NULL, &b));
  assert(!gw_signal_actions_representative(&actions, a));
  struct sigaction action = handler(handler_a, 0);
  assert(!gw_signal_actions_change(&actions, 56, &action, NULL));
  assert(!send(&actions, a, 56, 1).status);
  assert(!send(&actions, b, 56, 2).status);
  assert(!send(&actions, NULL, 56, 3).status);
  assert(!send(&actions, b, 57, 4).status);
  assert(send(&actions, a, 56, 5).status == EAGAIN);
  action.sa_handler = SIG_IGN;
  assert(!gw_signal_actions_change(&actions, 56, &action, NULL));
  assert(targets.inbox.queued == 1 && !gw_signal_target_pending(a));
  assert(!send(&actions, a, 56, 6).status);
  assert(targets.inbox.queued == 1);
  struct gw_signal_delivery delivery;
  assert(gw_signal_actions_deliver(&actions, b, &delivery) == 57);
  assert(delivery.information.si_value.sival_int == 4 &&
         delivery.source == GW_SIGNAL_THREAD &&
         delivery.selected.action.sa_handler == SIG_DFL);

  /* A synchronous disposition can coexist with a reserved asynchronous
   * channel; ignoring it still must not bypass that send reservation. */
  assert(!gw_signal_actions_change(&actions, SIGUSR2, &action, NULL));
  assert(send(&actions, a, SIGUSR2, 7).status == EBUSY);
  gw_signal_target_close(a);
  assert(send(&actions, a, 56, 8).status == ESRCH);
  assert(gw_signal_target_release(a) == EBUSY);

  action.sa_handler = handler_a;
  assert(!gw_signal_actions_change(&actions, SIGCHLD, &action, NULL));
  assert(!send(&actions, b, SIGCHLD, 9).status);
  assert(!send(&actions, NULL, SIGCHLD, 10).status);
  action.sa_handler = SIG_DFL;
  assert(!gw_signal_actions_change(&actions, SIGCHLD, &action, NULL));
  assert(!gw_signal_target_pending(b));
  assert(!send(&actions, b, SIGCHLD, 11).status);
  assert(!gw_signal_target_pending(b));

  gw_signal_actions_close(&actions);
  assert(!gw_signal_target_release(a));
  assert(send(&actions, b, 56, 12).status == ESRCH);
  assert(gw_signal_actions_deliver(&actions, b, &delivery) == -ESRCH);
  gw_signal_targets_close(&targets);
  assert(!gw_signal_target_release(b));
  assert(!gw_signal_targets_destroy(&targets));
}

struct race {
  struct gw_signal_actions *actions;
  struct gw_signal_target *target;
  _Atomic int *start;
  struct gw_signal_action selected;
};

static void *select_once(void *argument) {
  struct race *race = argument;
  while (!atomic_load_explicit(race->start, memory_order_acquire)) {
  }
  assert(!gw_signal_actions_select(race->actions, SIGUSR1, &race->selected));
  return NULL;
}

static void *send_many(void *argument) {
  struct race *race = argument;
  while (!atomic_load_explicit(race->start, memory_order_acquire)) {
  }
  for (int i = 0; i < 10000; ++i) {
    assert(!send(race->actions, race->target, 56, i).status);
  }
  return NULL;
}

static void registration_races(void) {
  struct gw_signal_targets targets;
  struct gw_signal_actions actions;
  gw_signal_targets_init(&targets, 0, 50000, NULL, NULL);
  gw_signal_actions_init(&actions, &targets, 0);
  struct gw_signal_target *target;
  assert(!gw_signal_target_open(&targets, 1, 1, 0, NULL, &target));
  assert(!gw_signal_actions_representative(&actions, target));
  struct sigaction action = handler(handler_a, SA_RESETHAND);
  assert(!gw_signal_actions_change(&actions, SIGUSR1, &action, NULL));
  _Atomic int start = 0;
  struct race races[4];
  pthread_t threads[4];
  for (int i = 0; i < 4; ++i) {
    races[i] = (struct race){
        .actions = &actions, .target = i % 2 ? target : NULL, .start = &start};
    assert(!pthread_create(&threads[i], NULL, select_once, &races[i]));
  }
  atomic_store_explicit(&start, 1, memory_order_release);
  int selected = 0;
  for (int i = 0; i < 4; ++i) {
    assert(!pthread_join(threads[i], NULL));
    selected += races[i].selected.action.sa_handler == handler_a;
  }
  assert(selected == 1);

  atomic_store(&start, 0);
  for (int i = 0; i < 4; ++i) {
    assert(!pthread_create(&threads[i], NULL, send_many, &races[i]));
  }
  atomic_store_explicit(&start, 1, memory_order_release);
  for (int i = 0; i < 1000; ++i) {
    action.sa_handler = i % 2 ? SIG_IGN : handler_b;
    assert(!gw_signal_actions_change(&actions, 56, &action, NULL));
  }
  for (int i = 0; i < 4; ++i) {
    assert(!pthread_join(threads[i], NULL));
  }
  action.sa_handler = SIG_IGN;
  assert(!gw_signal_actions_change(&actions, 56, &action, NULL));
  assert(!targets.inbox.queued && !gw_signal_target_pending(target));
  action.sa_handler = handler_b;
  assert(!gw_signal_actions_change(&actions, 56, &action, NULL));
  assert(!send(&actions, target, 56, 73).status);
  struct gw_signal_delivery delivery;
  assert(gw_signal_actions_deliver(&actions, target, &delivery) == 56);
  assert(delivery.selected.action.sa_handler == handler_b &&
         delivery.information.si_value.sival_int == 73);

  gw_signal_actions_close(&actions);
  gw_signal_targets_close(&targets);
  assert(!gw_signal_target_release(target));
  assert(!gw_signal_targets_destroy(&targets));
}

static void blocked_ignored(void) {
  struct gw_signal_targets targets;
  struct gw_signal_actions actions;
  gw_signal_targets_init(&targets, 0, 4, NULL, NULL);
  gw_signal_actions_init(&actions, &targets, 0);
  struct gw_signal_target *leader, *other;
  uint64_t bit = UINT64_C(1) << (56 - 1);
  assert(!gw_signal_target_open(&targets, 1, 1, bit, NULL, &leader));
  assert(!gw_signal_target_open(&targets, 2, 2, 0, NULL, &other));
  struct sigaction action = handler(SIG_IGN, 0);
  assert(!gw_signal_actions_change(&actions, 56, &action, NULL));
  assert(send(&actions, NULL, 56, 0).status == ENOTSUP);
  assert(!gw_signal_actions_representative(&actions, leader));
  assert(gw_signal_actions_representative(&actions, other) == EALREADY);

  assert(!send(&actions, leader, 56, 1).status);
  assert(!send(&actions, NULL, 56, 2).status);
  assert(targets.inbox.queued == 2);
  siginfo_t information;
  enum gw_signal_queue_source source;
  assert(gw_signal_target_take(leader, bit, &information, &source) == 56);
  assert(source == GW_SIGNAL_THREAD && information.si_value.sival_int == 1);
  assert(gw_signal_target_take(leader, bit, &information, &source) == 56);
  assert(source == GW_SIGNAL_PROCESS && information.si_value.sival_int == 2);

  /* A temporary unblocked mask retains the original mask's ignore filter. */
  assert(!gw_signal_target_masks(leader, 0, bit).status);
  assert(!send(&actions, leader, 56, 3).status);
  assert(!send(&actions, NULL, 56, 4).status);
  assert(targets.inbox.queued == 2);
  assert(!gw_signal_actions_change(&actions, 56, &action, NULL));
  assert(!targets.inbox.queued);

  assert(!gw_signal_target_mask(leader, 0).status);
  assert(!gw_signal_target_mask(other, bit).status);
  assert(!send(&actions, NULL, 56, 5).status);
  assert(!targets.inbox.queued); /* Leader decides, not a blocked peer. */
  assert(!send(&actions, other, 56, 6).status);
  assert(targets.inbox.queued == 1);
  action.sa_handler = handler_b;
  assert(!gw_signal_actions_change(&actions, 56, &action, NULL));
  assert(!gw_signal_target_mask(other, 0).status);
  struct gw_signal_delivery delivery;
  assert(gw_signal_actions_deliver(&actions, other, &delivery) == 56);
  assert(delivery.selected.action.sa_handler == handler_b &&
         delivery.information.si_value.sival_int == 6);

  /* Removing the leader from the dense vector cannot choose a new filter. */
  gw_signal_target_close(leader);
  action.sa_handler = SIG_IGN;
  assert(!gw_signal_actions_change(&actions, 56, &action, NULL));
  assert(!gw_signal_target_mask(other, bit).status);
  assert(!send(&actions, NULL, 56, 7).status);
  assert(!targets.inbox.queued);
  assert(gw_signal_target_release(leader) == EBUSY);
  gw_signal_actions_close(&actions);
  assert(!gw_signal_target_release(leader));
  gw_signal_targets_close(&targets);
  assert(!gw_signal_target_release(other));
  assert(!gw_signal_targets_destroy(&targets));
}

static void native_ignored_control(void) {
  const int numbers[] = {SIGUSR1, 56, SIGCHLD};
  for (size_t i = 0; i < sizeof numbers / sizeof *numbers; ++i) {
    int number = numbers[i];
    struct sigaction action = {.sa_handler = SIG_IGN}, previous;
    sigemptyset(&action.sa_mask);
    assert(!sigaction(number, &action, &previous));
    assert(!pthread_kill(pthread_self(), number));
    union sigval value = {.sival_int = 817};
    assert(!sigqueue(getpid(), number, value));
    sigset_t pending, eligible;
    assert(!sigpending(&pending) && sigismember(&pending, number));
    sigemptyset(&eligible);
    sigaddset(&eligible, number);
    struct timespec zero = {0};
    siginfo_t information;
    assert(sigtimedwait(&eligible, &information, &zero) == number);
    /* GNU sigtimedwait folds the kernel's SI_TKILL into SI_USER.
     * https://github.com/bminor/glibc/blob/glibc-2.44/sysdeps/unix/sysv/linux/sigtimedwait.c
     */
#ifdef __GLIBC__
    assert(information.si_code == SI_USER);
#else
    assert(information.si_code == SI_TKILL);
#endif
    assert(sigtimedwait(&eligible, &information, &zero) == number);
    assert(information.si_code == SI_QUEUE &&
           information.si_value.sival_int == 817);

    assert(!pthread_kill(pthread_self(), number));
    assert(!sigaction(number, &action, NULL));
    assert(!sigpending(&pending) && !sigismember(&pending, number));
    assert(!pthread_sigmask(SIG_UNBLOCK, &eligible, NULL));
    assert(!pthread_kill(pthread_self(), number));
    assert(!sigqueue(getpid(), number, value));
    assert(!sigpending(&pending) && !sigismember(&pending, number));
    assert(!pthread_sigmask(SIG_BLOCK, &eligible, NULL));
    assert(!sigaction(number, &previous, NULL));
  }
}

static void stop_continue_pending(void) {
  uint64_t stop = (UINT64_C(1) << (SIGTSTP - 1)) |
                  (UINT64_C(1) << (SIGTTIN - 1)) |
                  (UINT64_C(1) << (SIGTTOU - 1));
  uint64_t cont = UINT64_C(1) << (SIGCONT - 1);
  struct gw_signal_targets targets;
  struct gw_signal_actions actions;
  gw_signal_targets_init(&targets, 0, 4, NULL, NULL);
  gw_signal_actions_init(&actions, &targets, 0);
  struct gw_signal_target *a, *b;
  assert(!gw_signal_target_open(&targets, 1, 1, stop | cont, NULL, &a));
  assert(!gw_signal_target_open(&targets, 2, 2, stop | cont, NULL, &b));
  assert(!gw_signal_actions_representative(&actions, a));
  const int stops[] = {SIGTSTP, SIGTTIN, SIGTTOU};
  for (size_t i = 0; i < sizeof stops / sizeof *stops; ++i) {
    assert(!send(&actions, a, stops[i], 1).status);
    assert(!send(&actions, b, stops[i], 2).status);
    assert(!send(&actions, NULL, stops[i], 3).status);
  }
  assert((gw_signal_target_pending(a) & stop) == stop);
  assert(!send(&actions, NULL, SIGCONT, 4).status);
  assert(gw_signal_target_pending(a) == cont);
  assert(gw_signal_target_pending(b) == cont);
  assert(!send(&actions, a, SIGCONT, 5).status);
  assert(!send(&actions, b, SIGCONT, 6).status);
  assert(!send(&actions, a, SIGTSTP, 7).status);
  assert(!(gw_signal_target_pending(a) & cont));
  assert(!(gw_signal_target_pending(b) & cont));

  /* SIGCONT cancels stops even when unblocked and ignored, without queuing
   * itself. This is pending-state policy, not a process-resume implementation.
   */
  assert(!gw_signal_target_mask(a, 0).status);
  assert(!send(&actions, NULL, SIGCONT, 8).status);
  assert(!gw_signal_target_pending(a) && !gw_signal_target_pending(b));
  gw_signal_actions_close(&actions);
  gw_signal_targets_close(&targets);
  assert(!gw_signal_target_release(a) && !gw_signal_target_release(b));
  assert(!gw_signal_targets_destroy(&targets));

  sigset_t pending, eligible;
  sigemptyset(&eligible);
  sigaddset(&eligible, SIGCONT);
  for (size_t i = 0; i < sizeof stops / sizeof *stops; ++i) {
    assert(!pthread_kill(pthread_self(), stops[i]));
    assert(!kill(getpid(), stops[i]));
    sigaddset(&eligible, stops[i]);
  }
  assert(!kill(getpid(), SIGCONT));
  assert(!sigpending(&pending));
  for (size_t i = 0; i < sizeof stops / sizeof *stops; ++i) {
    assert(!sigismember(&pending, stops[i]));
  }
  assert(sigismember(&pending, SIGCONT));
  assert(!pthread_kill(pthread_self(), SIGCONT));
  assert(!pthread_kill(pthread_self(), SIGTSTP));
  assert(!sigpending(&pending) && !sigismember(&pending, SIGCONT));
  assert(sigismember(&pending, SIGTSTP));
  struct timespec zero = {0};
  assert(sigtimedwait(&eligible, NULL, &zero) == SIGTSTP);
  assert(sigtimedwait(&eligible, NULL, &zero) == -1 && errno == EAGAIN);
}

int main(void) {
  uint64_t all = UINT64_MAX;
  assert(!syscall(SYS_rt_sigprocmask, SIG_SETMASK, &all, NULL, sizeof all));
  snapshots_and_aliases();
  ignore_and_quota();
  registration_races();
  blocked_ignored();
  native_ignored_control();
  stop_continue_pending();
  puts(
      "{\"result\":\"PASS\",\"snapshots\":true,\"one_shot\":true,"
      "\"ignore_flush\":true,\"quota_recovered\":true,"
      "\"blocked_ignored\":true,\"native_ignored_control\":true,"
      "\"job_control_pending\":true,"
      "\"concurrent_senders\":4}");
}
