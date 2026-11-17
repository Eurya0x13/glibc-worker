#define _GNU_SOURCE
#include "private-threads.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

static _Thread_local volatile int private_tls = 127;
static _Atomic int waiting, cleaned, timed;
static int descriptors[2];

static void cleanup(void *argument) {
  assert(argument == &descriptors && private_tls == 127);
  atomic_store_explicit(&cleaned, 1, memory_order_release);
}

static void *cancelled(void *argument) {
  assert(argument == &descriptors && private_tls == 127);
  pthread_cleanup_push(cleanup, argument);
  atomic_store_explicit(&waiting, 1, memory_order_release);
  char byte;
  assert(read(descriptors[0], &byte, 1) == -1 && errno == EINTR);
  pthread_cleanup_pop(0);
  return NULL;
}

static void timeout(union sigval value) {
  assert(value.sival_ptr == &timed && private_tls == 127);
  atomic_store_explicit(&timed, 1, memory_order_release);
}

static void await(_Atomic int *state) {
  struct timespec start, now, pause = {.tv_nsec = 1000000};
  assert(!clock_gettime(CLOCK_MONOTONIC, &start));
  while (!atomic_load_explicit(state, memory_order_acquire)) {
    assert(!clock_gettime(CLOCK_MONOTONIC, &now));
    assert(now.tv_sec - start.tv_sec < 3);
    nanosleep(&pause, NULL);
  }
}

void test_private_threads(void) {
  assert(SIGRTMIN == 34 && SIGRTMAX == 62);
  private_tls = 717;
  struct sigaction action;
  for (int number = 63; number <= 64; ++number) {
    errno = 0;
    assert(sigaction(number, NULL, &action) == -1 && errno == EINVAL);
    assert(pthread_kill(pthread_self(), number) == EINVAL);
  }

  assert(!pipe(descriptors));
  pthread_t thread;
  assert(!pthread_create(&thread, NULL, cancelled, &descriptors));
  await(&waiting);
  assert(!pthread_cancel(thread));
  void *result;
  assert(!pthread_join(thread, &result) && result == PTHREAD_CANCELED);
  assert(atomic_load_explicit(&cleaned, memory_order_acquire));
  assert(!close(descriptors[0]) && !close(descriptors[1]));

  struct sigevent event = {.sigev_notify = SIGEV_THREAD,
                           .sigev_notify_function = timeout,
                           .sigev_value.sival_ptr = &timed};
  timer_t timer;
  assert(!timer_create(CLOCK_MONOTONIC, &event, &timer));
  struct itimerspec duration = {.it_value.tv_nsec = 1000000};
  assert(!timer_settime(timer, 0, &duration, NULL));
  await(&timed);
  assert(!timer_delete(timer));
  assert(private_tls == 717);
}
