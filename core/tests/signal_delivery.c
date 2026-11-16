#define _GNU_SOURCE
#include "signal-router.h"
#include "signals.h"
#include "nonlocal-mask.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t observing, hits, recurse;
static pthread_barrier_t barrier;
static volatile sig_atomic_t replacing, pending_replacing, old_hits, new_hits;
static int ignore_replacement;
static struct signal_slot *recipient;

static void send_signal(void) {
  if (!recipient) {
    assert(!raise(SIGTRAP));
    return;
  }
  siginfo_t information = {.si_signo = SIGTRAP, .si_code = SI_QUEUE};
  information.si_pid = getpid();
  information.si_uid = getuid();
  uint64_t previous = gw_nonlocal_mask_block();
  assert(!signal_host_defer(recipient, &information));
  gw_nonlocal_mask_restore(previous);
}

static void synchronize(void) {
  int result = pthread_barrier_wait(&barrier);
  assert(!result || result == PTHREAD_BARRIER_SERIAL_THREAD);
}

void signal_router_pending_observe(int number) {
  if (pending_replacing) {
    assert(number == SIGRTMIN + 4);
    pending_replacing = 0;
    synchronize();
    synchronize();
  }
}

void signal_router_delivery_observe(int number) {
  if (observing) {
    assert(number == SIGTRAP);
    struct sigaction action;
    assert(!sigaction(number, NULL, &action));
    /* The logical reset must precede unmasking and the user handler. Native
     * Linux consumes SA_RESETHAND while choosing the kernel disposition.
     * https://github.com/torvalds/linux/blob/v6.12/kernel/signal.c
     */
    assert(action.sa_handler == SIG_DFL);
  }
  if (replacing) {
    replacing = 0;
    synchronize();
    synchronize();
  }
}

static void once(int number) {
  assert(number == SIGTRAP);
  ++hits;
  if (recurse) {
    assert(!raise(number));
    assert(!"a repeated one-shot must select the native default action");
  }
}

static void old_action(int number) {
  assert(number == SIGTRAP);
  ++old_hits;
}

static void new_action(int number) {
  assert(number == SIGTRAP);
  ++new_hits;
}

static void *replace_action(void *unused) {
  (void)unused;
  synchronize();
  struct sigaction action = {.sa_handler = new_action};
  sigemptyset(&action.sa_mask);
  if (ignore_replacement) {
    struct sigaction ignore = {.sa_handler = SIG_IGN};
    sigemptyset(&ignore.sa_mask);
    assert(!sigaction(SIGTRAP, &ignore, NULL));
  }
  assert(!sigaction(SIGTRAP, &action, NULL));
  synchronize();
  return NULL;
}

static void selection_generation(int before_selection) {
  old_hits = new_hits = 0;
  assert(!pthread_barrier_init(&barrier, NULL, 2));
  pthread_t thread;
  assert(!pthread_create(&thread, NULL, replace_action, NULL));
  struct sigaction action = {.sa_handler = old_action};
  sigemptyset(&action.sa_mask);
  assert(!sigaction(SIGTRAP, &action, NULL));
  replacing = !before_selection;
  pending_replacing = before_selection;
  send_signal();
  assert(old_hits == 1 && new_hits == 0);
  send_signal();
  assert(old_hits == 1 && new_hits == 1);
  assert(!pthread_join(thread, NULL));
  assert(!pthread_barrier_destroy(&barrier));
}

int main(void) {
  struct rlimit limit = {0};
  assert(!setrlimit(RLIMIT_CORE, &limit));
  assert(!signal_router_init());
  assert(!signal_router_guest_faults());
  selection_generation(0);
  const int notifications[] = {SIGRTMIN,     SIGRTMIN + 1, SIGRTMIN + 2,
                               SIGRTMIN + 3, SIGRTMIN + 4, SIGRTMIN + 5};
  assert(!signal_router_defer_faults(notifications));
  struct exchange exchange = {.host_tid = syscall(SYS_gettid)};
  recipient = signal_acquire(&exchange);
  assert(recipient);
  selection_generation(0);
  ignore_replacement = 1;
  selection_generation(0);
  selection_generation(1);
  struct sigaction action = {.sa_handler = once,
                             .sa_flags = SA_RESETHAND | SA_NODEFER};
  sigemptyset(&action.sa_mask);
  assert(!sigaction(SIGTRAP, &action, NULL));
  observing = 1;
  send_signal();
  assert(hits == 1);
  assert(!signal_prepare_release(recipient, 0));
  signal_release(recipient);
  recipient = NULL;
  pid_t child = fork();
  assert(child >= 0);
  if (!child) {
    hits = 0;
    recurse = 1;
    assert(!sigaction(SIGTRAP, &action, NULL));
    raise(SIGTRAP);
    _exit(1);
  }
  int status;
  assert(waitpid(child, &status, 0) == child);
  assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGTRAP);
  assert(!signal_router_restore());
  puts(
      "{\"result\":\"PASS\",\"reset_before_unmask\":true,"
      "\"selected_generation\":true,\"nested_reset_default\":true,"
      "\"deferred_selection_generation\":true,"
      "\"ignore_after_selection\":true,\"kernel_selected_generation\":true}");
}
