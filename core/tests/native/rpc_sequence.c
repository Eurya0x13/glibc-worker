#define _GNU_SOURCE
#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include "rpc_sequence.h"
#include "wait_event.h"

static struct {
  _Atomic int request, response;
  struct fb_event server_event, client_event;
  unsigned input, output;
} channel;

static const unsigned rounds = 8192;

static void *server(void *unused) {
  (void)unused;

  for (unsigned i = 0; i < rounds; i++) {
    int request;
    for (;;) {
      uint32_t sample = fb_event_sample(&channel.server_event);
      request = atomic_load_explicit(&channel.request, memory_order_acquire);
      if (request != atomic_load(&channel.response)) {
        break;
      }
      fb_event_wait_active(&channel.server_event, sample);
    }

    assert(channel.input == i);
    channel.output = i ^ 0x5a5a;
    atomic_store_explicit(&channel.response, request, memory_order_release);
    fb_event_notify(&channel.client_event);
  }

  return NULL;
}

static void exercise(int start) {
  atomic_store(&channel.request, start);
  atomic_store(&channel.response, start);

  pthread_t thread;
  assert(!pthread_create(&thread, NULL, server, NULL));

  int seq = start;
  for (unsigned i = 0; i < rounds; i++) {
    seq = gw_rpc_next_sequence(seq);
    channel.input = i;
    atomic_store_explicit(&channel.request, seq, memory_order_release);
    fb_event_notify(&channel.server_event);
    fb_event_notify(&channel.server_event); /* Redundant/stale notification. */

    for (;;) {
      uint32_t sample = fb_event_sample(&channel.client_event);
      if (atomic_load_explicit(&channel.response, memory_order_acquire) ==
          seq) {
        break;
      }
#if GW_RPC_READY_POLL
      fb_event_wait_active_response(&channel.client_event, sample,
                                    &channel.response, seq);
#else
      fb_event_wait_active(&channel.client_event, sample);
#endif
    }

    assert(channel.output == (i ^ 0x5a5a));
  }

  assert(!pthread_join(thread, NULL));
}

int main(void) {
  assert(gw_rpc_next_sequence(INT_MAX) == INT_MIN);
  assert(gw_rpc_next_sequence(-1) == 0);
  assert(gw_rpc_next_sequence(INT_MIN) == INT_MIN + 1);

  exercise(INT_MAX - 4);
  exercise(-4);

  puts(
      "{\"result\":\"PASS\",\"roundtrips\":16384,\"signed_wrap\":true,\"zero_"
      "sequence\":true}");
}
