#define _GNU_SOURCE
#include "signal-pending.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>

static struct gw_signal_pending pending;

static siginfo_t message(int number, int value) {
  siginfo_t information = {.si_signo = number, .si_code = SI_QUEUE};
  information.si_pid = 719;
  information.si_uid = 193;
  information.si_value.sival_int = value;
  return information;
}

static void *produce(void *argument) {
  int producer = (int)(intptr_t)argument;
  for (int sequence = 0; sequence < 1000; ++sequence) {
    siginfo_t information = message(40, producer * 1000 + sequence);
    assert(!gw_signal_pending_put(&pending, &information, 10000));
  }
  return NULL;
}

int main(void) {
  sigset_t all;
  sigfillset(&all);
  assert(!pthread_sigmask(SIG_BLOCK, &all, NULL));
  siginfo_t information = message(SIGUSR1, 1), observed;
  errno = EDOM;
  assert(!gw_signal_pending_put(&pending, &information, 10000));
  information.si_value.sival_int = 2;
  assert(!gw_signal_pending_put(&pending, &information, 10000));
  assert(errno == EDOM);
  assert(gw_signal_pending_take(&pending, UINT64_MAX, &observed) == SIGUSR1);
  assert(observed.si_value.sival_int == 1 && observed.si_pid == 719 &&
         observed.si_uid == 193 && observed.si_code == SI_QUEUE);
  assert(!gw_signal_pending_bits(&pending));
  pthread_t threads[8];
  for (intptr_t i = 0; i < 8; ++i) {
    assert(!pthread_create(&threads[i], NULL, produce, (void *)i));
  }
  for (int i = 0; i < 8; ++i) {
    assert(!pthread_join(threads[i], NULL));
  }
  int sequences[8] = {0};
  for (int i = 0; i < 8000; ++i) {
    assert(gw_signal_pending_take(&pending, UINT64_MAX, &observed) == 40);
    int value = observed.si_value.sival_int;
    assert(value / 1000 < 8 && sequences[value / 1000]++ == value % 1000);
  }
  assert(!gw_signal_pending_take(&pending, UINT64_MAX, &observed));
  information = message(41, 3);
  assert(!gw_signal_pending_put(&pending, &information, 1));
  assert(gw_signal_pending_put(&pending, &information, 1) == EAGAIN);
  assert(!gw_signal_pending_take(&pending, UINT64_C(1) << 39, &observed));
  information = message(SIGUSR1, 4);
  assert(!gw_signal_pending_put(&pending, &information, 1));
  assert(gw_signal_pending_take(&pending, UINT64_MAX, &observed) == SIGUSR1);
  gw_signal_pending_discard(&pending, 41);
  assert(!pending.queued && !gw_signal_pending_bits(&pending));
  information = message(41, 5);
  assert(!gw_signal_pending_put(&pending, &information, 1));
  gw_signal_pending_close(&pending);
  assert(!pending.queued && !gw_signal_pending_bits(&pending));
  assert(gw_signal_pending_put(&pending, &information, 1) == ESRCH);
  gw_signal_pending_destroy(&pending);
  puts(
      "{\"result\":\"PASS\",\"standard_coalescing\":true,"
      "\"fifo_messages\":8000,\"producers\":8,\"quota\":true,"
      "\"close_rejects\":true}");
}
