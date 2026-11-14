#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include "wait_event.h"

#if GW_RPC_PROFILE
_Thread_local struct gw_rpc_row *gw_rpc_current;
_Thread_local struct gw_rpc_scope *gw_rpc_scope_current;
#endif
#define CHECK(test)                                                      \
  do {                                                                   \
    if (!(test)) {                                                       \
      fprintf(stderr, "ready-poll assertion at line %d: %s\n", __LINE__, \
              #test);                                                    \
      abort();                                                           \
    }                                                                    \
  } while (0)

static struct fb_event event;
static _Atomic int response, release_sender, notified;
static int publish_response;

static void *sender(void *unused) {
  (void)unused;
  struct timespec pause = {.tv_nsec = 1000000};
  /* Model a publisher descheduled after the result store but before notify.
   * A bounded watchdog also releases the legacy event-only waiter. */
  for (unsigned i = 0; i < 200 && !atomic_load(&release_sender); i++) {
    nanosleep(&pause, NULL);
  }

  if (publish_response) {
    atomic_store_explicit(&response, 7, memory_order_release);
  }
  atomic_store(&notified, 1);
  fb_event_notify(&event);
  return NULL;
}

static int exercise(int initial_response, int publish) {
  event = (struct fb_event){0};
  atomic_store(&response, initial_response);
  atomic_store(&release_sender, 0);
  atomic_store(&notified, 0);
  publish_response = publish;

  pthread_t thread;
  CHECK(!pthread_create(&thread, NULL, sender, NULL));
  uint32_t observed = fb_event_sample(&event);
#if GW_RPC_READY_POLL
  fb_event_wait_active_response(&event, observed, &response, 7);
#else
  fb_event_wait_active(&event, observed);
#endif
  int waited_for_notify = atomic_load(&notified);
  atomic_store(&release_sender, 1);
  CHECK(!pthread_join(thread, NULL));
  return waited_for_notify;
}

int main(void) {
  CHECK(exercise(7, 0) == !GW_RPC_READY_POLL);
  /* An older response generation cannot satisfy the current call. */
  CHECK(exercise(6, 1));
  CHECK(atomic_load(&response) == 7);
  /* An event without completion must return to the caller's callback pump. */
  CHECK(exercise(6, 0));
  CHECK(atomic_load(&response) == 6);

  printf(
      "READY_POLL enabled=%d profile=%d spin=%d published_gap=PASS "
      "stale_generation=PASS "
      "callback_event=PASS result=PASS\n",
      GW_RPC_READY_POLL, GW_RPC_PROFILE, GW_RPC_SPIN_NS);
  return 0;
}
