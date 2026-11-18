#define _GNU_SOURCE
#include "signal-relay.h"
#include "signal-router.h"
#include "signals.h"
#include "nonlocal-mask.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <ucontext.h>

static _Thread_local int canary = 7193;
static _Atomic unsigned hits;
static _Atomic long dispatcher_tid;
static pthread_barrier_t selected, replaced;
static int replacing;

enum relay_window { AT_CREATE = 1, AT_JOIN };
static _Thread_local enum relay_window window;
static pthread_barrier_t entering_window, leaving_window;

static void barrier(pthread_barrier_t *value) {
  int result = pthread_barrier_wait(value);
  assert(!result || result == PTHREAD_BARRIER_SERIAL_THREAD);
}

int __real_pthread_create(pthread_t *, const pthread_attr_t *,
                          void *(*)(void *), void *);
int __real_pthread_join(pthread_t, void **);

int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attributes,
                          void *(*entry)(void *), void *argument) {
  if (window == AT_CREATE) {
    barrier(&entering_window);
    barrier(&leaving_window);
  }
  return __real_pthread_create(thread, attributes, entry, argument);
}

int __wrap_pthread_join(pthread_t thread, void **result) {
  if (window == AT_JOIN) {
    barrier(&entering_window);
    barrier(&leaving_window);
  }
  return __real_pthread_join(thread, result);
}

static void *transition(void *argument) {
  window = *(const enum relay_window *)argument;
  int error = window == AT_CREATE ? signal_host_relay_start()
                                  : signal_host_relay_stop();
  assert(!error);
  return NULL;
}

static void check_restore_window(enum relay_window paused) {
  pthread_t thread;
  assert(!pthread_create(&thread, NULL, transition, &paused));
  barrier(&entering_window);
  assert(signal_host_relay_active() && !signal_host_relay_running());
  assert(signal_host_relay_start() == EALREADY);
  assert(signal_host_relay_stop() == EBUSY);
  assert(signal_router_restore() == EBUSY);
  barrier(&leaving_window);
  assert(!pthread_join(thread, NULL));
}

static void receive(int number, siginfo_t *information, void *frame) {
  assert(number == SIGURG && canary == 7193);

  long here = syscall(SYS_gettid);
  long old = atomic_load(&dispatcher_tid);
  if (!old) {
    atomic_store(&dispatcher_tid, here);
  } else {
    assert(old == here);
  }

  assert(information->si_code == SI_QUEUE || information->si_code == SI_USER);
  assert(information->si_pid == getpid() && information->si_uid == getuid());
  if (information->si_code == SI_QUEUE) {
    assert(information->si_value.sival_int != here);
  }
  assert(frame);
  assert(signal_host_relay_stop() == EDEADLK);

  sigset_t mask;
  assert(!pthread_sigmask(SIG_SETMASK, NULL, &mask));
  assert(sigismember(&mask, SIGURG));

  if (replacing) {
    barrier(&selected);
    barrier(&replaced);
  }

  atomic_fetch_add(&hits, 1);
  errno = ERANGE;
}

static void *sender(void *unused) {
  /* Unit-test registration exercises the genuine-thread entry; this file
   * does not pretend its Host pthread is a second GNU libc runtime. */
  uint64_t mask = gw_nonlocal_mask_block();
  signal_guest_lifecycle(1);
  gw_nonlocal_mask_restore(mask);

  if (unused) {
    sigset_t waiting;
    assert(!pthread_sigmask(SIG_SETMASK, NULL, &waiting));
    sigdelset(&waiting, SIGURG);
    barrier(&selected);
    assert(sigsuspend(&waiting) == -1 && errno == EINTR);
    goto leave;
  }

  siginfo_t information = {.si_signo = SIGURG, .si_code = SI_QUEUE};
  information.si_pid = getpid();
  information.si_uid = getuid();
  information.si_value.sival_int = (int)syscall(SYS_gettid);

  errno = EDOM;
  assert(!syscall(SYS_rt_tgsigqueueinfo, getpid(), syscall(SYS_gettid), SIGURG,
                  &information));
  assert(errno == EDOM && canary == 7193);

leave:
  mask = gw_nonlocal_mask_block();
  signal_guest_lifecycle(0);
  gw_nonlocal_mask_restore(mask);
  return NULL;
}

int main(void) {
  assert(signal_host_relay_start() == EINVAL);
  assert(!signal_router_init());
  assert(!pthread_barrier_init(&entering_window, NULL, 2));
  assert(!pthread_barrier_init(&leaving_window, NULL, 2));
  check_restore_window(AT_CREATE);
  assert(signal_host_relay_start() == EALREADY);
  assert(signal_router_restore() == EBUSY);

  struct sigaction action = {.sa_sigaction = receive, .sa_flags = SA_SIGINFO};
  sigemptyset(&action.sa_mask);
  assert(!sigaction(SIGURG, &action, NULL));
  assert(!pthread_barrier_init(&selected, NULL, 2));
  assert(!pthread_barrier_init(&replaced, NULL, 2));

  pthread_t thread;
  replacing = 1;
  assert(!pthread_create(&thread, NULL, sender, NULL));
  barrier(&selected);
  assert(signal_host_relay_busy());
  assert(signal_host_relay_stop() == EBUSY);

  struct sigaction ignore = {.sa_handler = SIG_IGN};
  sigemptyset(&ignore.sa_mask);
  assert(!sigaction(SIGURG, &ignore, NULL));
  barrier(&replaced);
  assert(!pthread_join(thread, NULL));
  replacing = 0;
  assert(atomic_load(&hits) == 1);

  assert(!sigaction(SIGURG, &action, NULL));
  pthread_t threads[32];
  for (unsigned i = 0; i < 32; ++i) {
    assert(!pthread_create(&threads[i], NULL, sender, NULL));
  }
  for (unsigned i = 0; i < 32; ++i) {
    assert(!pthread_join(threads[i], NULL));
  }
  assert(atomic_load(&hits) == 33);

  sigset_t blocked, inherited;
  sigemptyset(&blocked);
  sigaddset(&blocked, SIGURG);
  assert(!pthread_sigmask(SIG_BLOCK, &blocked, &inherited));
  assert(!pthread_create(&thread, NULL, sender, (void *)1));
  barrier(&selected);
  /* The only eligible recipient is the registered GNU-entry test thread:
   * main and the real Host dispatcher both keep this signal blocked. */
  assert(!kill(getpid(), SIGURG));
  assert(!pthread_join(thread, NULL));
  assert(!pthread_sigmask(SIG_SETMASK, &inherited, NULL));
  assert(atomic_load(&hits) == 34);

  assert(!signal_host_relay_busy());
  check_restore_window(AT_JOIN);
  assert(!signal_router_restore());
  assert(!pthread_barrier_destroy(&entering_window));
  assert(!pthread_barrier_destroy(&leaving_window));
  assert(!pthread_barrier_destroy(&selected));
  assert(!pthread_barrier_destroy(&replaced));

  puts(
      "{\"result\":\"PASS\",\"host_tls\":true,\"original_siginfo\":true,"
      "\"different_thread\":true,\"concurrent_recipients\":true,"
      "\"busy_stop\":true,\"selected_generation\":true,"
      "\"process_directed\":true,\"startup_and_join_windows\":true}");
}
