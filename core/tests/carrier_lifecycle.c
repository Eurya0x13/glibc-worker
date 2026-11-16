#define _GNU_SOURCE
#include "signals.h"
#include "signal-router.h"
#include "nonlocal-mask.h"
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t delivered;

static void receive(int number) {
  assert(number == 50);
  ++delivered;
}

static void receive_unreserved(int number) {
  assert(number == SIGUSR1);
  ++delivered;
}

static void unreserved_registration(void) {
  signal_router_backend native =
      (signal_router_backend)dlsym(RTLD_NEXT, "sigaction");
  assert(native);
  struct sigaction action = {.sa_handler = receive_unreserved}, inherited;
  sigemptyset(&action.sa_mask);
  assert(!native(SIGUSR1, &action, &inherited));
  assert(!signal_router_init());
  assert(signal_router_guest_ready() == ENOTSUP);
  assert(!signal_router_adopt());
  assert(!signal_router_guest_ready());
  assert(!(signal_router_reserved() & (UINT64_C(1) << (SIGUSR1 - 1))));
  struct sigaction logical, physical;
  assert(!sigaction(SIGUSR1, NULL, &logical));
  assert(!native(SIGUSR1, NULL, &physical));
  assert(logical.sa_handler == receive_unreserved);
  assert(physical.sa_handler != receive_unreserved);
  sigset_t set, mask;
  sigemptyset(&set);
  sigaddset(&set, SIGUSR1);
  assert(!pthread_sigmask(SIG_UNBLOCK, &set, &mask));
  int before = delivered;
  assert(!raise(SIGUSR1) && delivered == before + 1);
  assert(!signal_router_restore());
  assert(!native(SIGUSR1, NULL, &physical));
  assert(physical.sa_handler == receive_unreserved);
  assert(!native(SIGUSR1, &inherited, NULL));
  assert(!pthread_sigmask(SIG_SETMASK, &mask, NULL));
}

static void deferred_retirement(void) {
  sigset_t inherited;
  assert(!pthread_sigmask(SIG_SETMASK, NULL, &inherited));
  sigset_t private_mask;
  sigemptyset(&private_mask);
  sigaddset(&private_mask, SIGRTMIN);
  assert(!pthread_sigmask(SIG_BLOCK, &private_mask, NULL));
  uint64_t initial;
  assert(!syscall(SYS_rt_sigprocmask, SIG_SETMASK, NULL, &initial, 8));
  assert(!signal_router_init());
  const int notifications[] = {SIGRTMIN,     SIGRTMIN + 1, SIGRTMIN + 2,
                               SIGRTMIN + 3, SIGRTMIN + 4, SIGRTMIN + 5};
  assert(!signal_router_defer_faults(notifications));
  struct exchange exchange = {.host_tid = syscall(SYS_gettid)};
  struct signal_slot *slot = signal_acquire(&exchange);
  assert(slot && slot->deferred_active);
  gw_nonlocal_mask_block();
  siginfo_t information = {.si_signo = SIGTRAP, .si_code = SI_QUEUE};
  information.si_value.sival_int = 7193;
  assert(!gw_signal_deferred_put(slot->deferred, &information));
  assert(signal_host_retirement_busy());
  assert(signal_deferred_recipients() == 1);
  assert(signal_router_restore() == EBUSY);
  assert(signal_prepare_release(slot, 0) == EBUSY);
  assert(gw_signal_pending_bits(&slot->deferred->pending));
  assert(!signal_prepare_release(slot, 1));
  assert(!signal_host_retirement_busy());
  signal_release(slot);
  assert(!signal_deferred_recipients());
  assert(!signal_router_restore());
  uint64_t restored, notification_bits = 0;
  assert(!syscall(SYS_rt_sigprocmask, SIG_SETMASK, NULL, &restored, 8));
  for (unsigned i = 0; i < sizeof notifications / sizeof notifications[0];
       ++i) {
    notification_bits |= UINT64_C(1) << (notifications[i] - 1);
  }
  assert((restored & notification_bits) == (initial & notification_bits));
  assert(!pthread_sigmask(SIG_SETMASK, &inherited, NULL));
  /* A later router lifetime without a channel must not reuse the previous
   * owner's private mask. In particular it must not reblock SIGRTMIN. */
  assert(!signal_router_init());
  assert(!pthread_sigmask(SIG_UNBLOCK, &private_mask, NULL));
  assert(!signal_router_restore());
  assert(!pthread_sigmask(SIG_SETMASK, NULL, &private_mask));
  assert(!sigismember(&private_mask, SIGRTMIN));
  assert(!pthread_sigmask(SIG_SETMASK, &inherited, NULL));
}

static void ignored_host_fault(void) {
  for (int routed = 0; routed < 2; ++routed) {
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
      const struct rlimit no_core = {0};
      assert(!setrlimit(RLIMIT_CORE, &no_core));
      alarm(2); /* An ignored routed fault must not spin indefinitely. */
      if (routed) {
        assert(!signal_router_init());
        assert(!signal_router_guest_faults());
      }
      struct sigaction ignore = {.sa_handler = SIG_IGN};
      sigemptyset(&ignore.sa_mask);
      assert(!sigaction(SIGSEGV, &ignore, NULL));
      sigset_t mask;
      sigemptyset(&mask);
      sigaddset(&mask, SIGSEGV);
      assert(!pthread_sigmask(SIG_UNBLOCK, &mask, NULL));
      volatile unsigned char *page =
          mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      assert(page != MAP_FAILED);
      *page = 1;
      _exit(42);
    }
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
  }
}

int main(void) {
  assert(!signal_router_init());
  sigset_t reserved, inherited, observed, pending;
  sigemptyset(&reserved);
  sigaddset(&reserved, 50);
  sigaddset(&reserved, 51);
  assert(!pthread_sigmask(SIG_BLOCK, &reserved, &inherited));
  struct sigaction action = {.sa_handler = receive}, old_action;
  sigemptyset(&action.sa_mask);
  assert(!sigaction(50, &action, &old_action));
  stack_t old_stack, after;
  assert(!sigaltstack(NULL, &old_stack));
  struct exchange exchange = {.host_tid = syscall(SYS_gettid)};
  struct signal_slot *slot = signal_acquire(&exchange);
  assert(slot);
  assert(!pthread_sigmask(SIG_SETMASK, NULL, &observed));
  assert(sigismember(&observed, 50) && sigismember(&observed, 51));
  sigset_t changed;
  sigemptyset(&changed);
  sigaddset(&changed, SIGUSR1);
  assert(!pthread_sigmask(SIG_BLOCK, &changed, NULL));
  assert(!syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), 50));
  signal_release(slot);
  assert(!pthread_sigmask(SIG_SETMASK, NULL, &observed));
  assert(sigismember(&observed, SIGUSR1));
  assert(sigismember(&observed, 50) && sigismember(&observed, 51));
  assert(!sigpending(&pending) && sigismember(&pending, 50));
  assert(!delivered);
  assert(!sigaltstack(NULL, &after));
  assert(after.ss_sp == old_stack.ss_sp && after.ss_size == old_stack.ss_size &&
         after.ss_flags == old_stack.ss_flags);
  sigemptyset(&changed);
  sigaddset(&changed, 50);
  assert(!pthread_sigmask(SIG_UNBLOCK, &changed, NULL));
  assert(delivered == 1);
  assert(!sigaction(50, &old_action, NULL));
  assert(!pthread_sigmask(SIG_SETMASK, &inherited, NULL));
  assert(!signal_router_restore());
  deferred_retirement();
  unreserved_registration();
  ignored_host_fault();
  puts(
      "{\"result\":\"PASS\",\"host_masks\":true,\"pending_retained\":true,"
      "\"alternate_stack_restored\":true,\"live_retirement_busy\":true,"
      "\"exiting_recipient_discards\":true,\"private_mask_restored\":true,"
      "\"unreserved_host_route\":true,\"unadopted_handler_rejected\":true,"
      "\"host_ignored_fault_native_comparison\":true}");
}
