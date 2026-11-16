#define _GNU_SOURCE
#include "signal-deferred.h"
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

static struct gw_signal_deferred deferred;
static volatile sig_atomic_t guest, faults, deliveries, failures;
static siginfo_t delivered;
static sigset_t all;

static uint64_t bit(int number) {
  return UINT64_C(1) << (number - 1);
}

static sigset_t mask(uint64_t bits) {
  sigset_t result;
  sigemptyset(&result);
  memcpy(&result, &bits, sizeof bits);
  return result;
}

static void set_mask(uint64_t logical, int in_guest) {
  sigset_t physical =
      mask(gw_signal_deferred_mask(&deferred, logical, in_guest));
  assert(!pthread_sigmask(SIG_SETMASK, &physical, NULL));
}

static void receive(int number, siginfo_t *information, void *context) {
  int saved_errno = errno;
  if (number == SIGTRAP && information->si_code > 0) {
    if (!guest) {
      ++failures;
    }
    ++faults;
#if defined(__aarch64__)
    ((ucontext_t *)context)->uc_mcontext.pc += 4;
#else
    (void)context;
#endif
  } else if (number == SIGTRAP) {
    if (!guest || gw_signal_deferred_put(&deferred, information)) {
      ++failures;
    }
  } else {
    int original = gw_signal_deferred_take(&deferred, number, &delivered);
    if (original) {
      if (guest || original != SIGTRAP) {
        ++failures;
      }
      ++deliveries;
    }
  }
  errno = saved_errno;
}

static void send_value(int value) {
  siginfo_t information = {.si_signo = SIGTRAP, .si_code = SI_QUEUE};
  information.si_pid = getpid();
  information.si_uid = getuid();
  information.si_value.sival_int = value;
  assert(!syscall(SYS_rt_tgsigqueueinfo, getpid(), syscall(SYS_gettid), SIGTRAP,
                  &information));
}

static int consume(int notification, siginfo_t *original) {
  sigset_t wanted;
  sigemptyset(&wanted);
  sigaddset(&wanted, notification);
  siginfo_t wake;
  struct timespec timeout = {.tv_sec = 1};
  assert(sigtimedwait(&wanted, &wake, &timeout) == notification);
  return gw_signal_deferred_take(&deferred, notification, original);
}

static void check_value(const siginfo_t *information, int value) {
  assert(information->si_signo == SIGTRAP && information->si_code == SI_QUEUE);
  assert(information->si_pid == getpid() && information->si_uid == getuid());
  assert(information->si_value.sival_int == value);
}

static void enter_guest(void) {
  guest = 1;
  set_mask(0, 1);
}

static void leave_guest_blocked(void) {
  assert(!pthread_sigmask(SIG_SETMASK, &all, NULL));
  guest = 0;
}

int main(void) {
  sigset_t old;
  sigfillset(&all);
  assert(!pthread_sigmask(SIG_SETMASK, &all, &old));
  int notifications[GW_SIGNAL_FAULT_LANES];
  for (int i = 0; i < GW_SIGNAL_FAULT_LANES; ++i) {
    notifications[i] = SIGRTMIN + i;
  }
  assert(!gw_signal_deferred_init(&deferred, notifications));
  struct sigaction action = {.sa_sigaction = receive, .sa_flags = SA_SIGINFO};
  action.sa_mask = all;
  struct sigaction saved[GW_SIGNAL_FAULT_LANES], trap;
  assert(!sigaction(SIGTRAP, &action, &trap));
  for (int i = 0; i < GW_SIGNAL_FAULT_LANES; ++i) {
    assert(!sigaction(notifications[i], &action, &saved[i]));
  }
  int notification = notifications[4];

  /* Real synchronous faults remain deliverable while the Host's corresponding
   * logical signal is blocked. Async arrivals must remain pending for Host.
   * This isolates notification mechanics; it does not switch libc TLS.
   */
  enter_guest();
#if defined(__x86_64__)
  __asm__ volatile("int3" ::: "memory");
#elif defined(__aarch64__)
  __asm__ volatile("brk #0" ::: "memory");
#endif
  send_value(101);
  send_value(102);
  leave_guest_blocked();
  set_mask(bit(SIGTRAP), 0);
  assert(faults == 1 && !deliveries && !failures);
  sigset_t kernel;
  assert(!sigpending(&kernel));
  uint64_t pending_bits;
  memcpy(&pending_bits, &kernel, sizeof pending_bits);
  assert(sigismember(&kernel, notification) == 1);
  assert(!pthread_sigmask(SIG_SETMASK, &all, NULL));
  assert(gw_signal_deferred_visible(&deferred, pending_bits) == bit(SIGTRAP));
  errno = EDOM;
  set_mask(0, 0);
  assert(deliveries == 1 && !failures && errno == EDOM);
  check_value(&delivered, 101);

  enter_guest();
  send_value(201);
  leave_guest_blocked();
  siginfo_t original;
  assert(consume(notification, &original) == SIGTRAP);
  check_value(&original, 201);

  /* Ignoring a logical signal discards its payload, retaining the kernel wake
   * as a tombstone. A later send uses fresh metadata, even with that wake left.
   */
  enter_guest();
  send_value(301);
  leave_guest_blocked();
  gw_signal_deferred_discard(&deferred, SIGTRAP);
  assert(!gw_signal_deferred_visible(&deferred, bit(notification)));
  assert(!consume(notification, &original));
  enter_guest();
  send_value(401);
  leave_guest_blocked();
  gw_signal_deferred_discard(&deferred, SIGTRAP);
  enter_guest();
  send_value(402);
  leave_guest_blocked();
  assert(consume(notification, &original) == SIGTRAP);
  check_value(&original, 402);

  /* signalfd observes a real kernel notification; a future adapter must decode
   * the original and filter tombstone readiness before exposing it to users.
   */
  sigset_t wanted = mask(bit(notification));
  int descriptor = signalfd(-1, &wanted, SFD_NONBLOCK | SFD_CLOEXEC);
  assert(descriptor >= 0);
  enter_guest();
  send_value(501);
  leave_guest_blocked();
  struct pollfd poller = {.fd = descriptor, .events = POLLIN};
  assert(poll(&poller, 1, 1000) == 1 && (poller.revents & POLLIN));
  struct signalfd_siginfo wake;
  assert(read(descriptor, &wake, sizeof wake) == sizeof wake);
  assert(wake.ssi_signo == (unsigned)notification);
  assert(gw_signal_deferred_take(&deferred, notification, &original) ==
         SIGTRAP);
  check_value(&original, 501);
  assert(poll(&poller, 1, 0) == 0);
  assert(!close(descriptor));

  /* If sending the notification fails, the accepted original remains owned
   * by this queue. Retrying arm rather than put preserves its first payload.
   */
  siginfo_t retry = {.si_signo = SIGTRAP, .si_code = SI_QUEUE};
  retry.si_pid = getpid();
  retry.si_uid = getuid();
  retry.si_value.sival_int = 551;
  pid_t recipient = deferred.recipient;
  deferred.recipient = 0;
  errno = ERANGE;
  assert(gw_signal_deferred_put(&deferred, &retry) == EINVAL);
  assert(errno == ERANGE && !deferred.armed);
  assert(gw_signal_pending_bits(&deferred.pending) == bit(SIGTRAP));
  deferred.recipient = recipient;
  assert(!gw_signal_deferred_arm(&deferred) && errno == ERANGE);
  assert(consume(notification, &original) == SIGTRAP);
  check_value(&original, 551);

  enter_guest();
  send_value(601);
  leave_guest_blocked();
  assert(gw_signal_deferred_close(&deferred) == EBUSY);
  assert(consume(notification, &original) == SIGTRAP);
  assert(!gw_signal_deferred_close(&deferred));
  assert(gw_signal_deferred_put(&deferred, &original) == ESRCH);
  assert(!pthread_sigmask(SIG_SETMASK, &all, NULL));
  assert(!sigaction(SIGTRAP, &trap, NULL));
  for (int i = 0; i < GW_SIGNAL_FAULT_LANES; ++i) {
    assert(!sigaction(notifications[i], &saved[i], NULL));
  }
  assert(!pthread_sigmask(SIG_SETMASK, &old, NULL));
  puts(
      "{\"result\":\"PASS\",\"hardware_fault\":true,"
      "\"host_deferred\":true,\"metadata_coalescing\":true,"
      "\"sigtimedwait\":true,\"signalfd\":true,"
      "\"cancel_and_fresh_send\":true,\"notification_retry\":true,"
      "\"close_guard\":true}");
}
