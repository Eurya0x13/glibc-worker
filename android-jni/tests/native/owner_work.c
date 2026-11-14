/* Production fg_pump with a no-op native executor: isolate owner maintenance,
   not JNI/RPC latency. Real pthread workers exercise lifecycle transitions. */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "guest_internal.h"

static __thread unsigned long executions;

void fg_execute(struct fb_call *call) {
  assert(call);
  executions++;
}

static uint64_t clock_ns(clockid_t clock) {
  struct timespec t;
  assert(!clock_gettime(clock, &t));
  return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

static void allocate_capacity(int capacity) {
  G = calloc(1, sizeof *G);
  assert(G && capacity >= FB_CONTEXTS);
  for (int i = FB_CONTEXTS; i < capacity; i += FB_PAGE_ITEMS) {
    assert(fb_pages_at(&G->contexts, (unsigned)(i - FB_CONTEXTS),
                       sizeof(struct fb_context), 1));
  }
  atomic_store(&G->context_capacity, capacity);
  atomic_store(&G->running, 1);
  gid = gdepth = 0;
}

static void benchmark(int capacity) {
  allocate_capacity(capacity);
  /* Model a retained high-water mark after all auxiliary threads exited.
     Touch every context before timing, rather than charging first faults. */
  for (int i = 1; i < capacity; i++) {
    atomic_store(&fb_context_at(G, i)->state, FB_FREE);
  }
  const unsigned calls = 2000;
  for (int trial = 0; trial < 11; trial++) {
    unsigned long before = executions;
    uint64_t cpu = clock_ns(CLOCK_THREAD_CPUTIME_ID);
    for (unsigned i = 0; i < calls; i++) {
      fg_pump();
    }
    cpu = clock_ns(CLOCK_THREAD_CPUTIME_ID) - cpu;
    assert(executions - before == calls && cpu);
    printf("{\"capacity\":%d,\"trial\":%d,\"calls\":%u,\"cpu_ns\":%llu}\n",
           capacity, trial, calls, (unsigned long long)cpu);
  }
}

#ifndef GW_OWNER_BASELINE
static int fail_create, fail_private;
int __real_pthread_create(pthread_t *, const pthread_attr_t *,
                          void *(*)(void *), void *);
void *__real_calloc(size_t, size_t);

int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                          void *(*start)(void *), void *arg) {
  return fail_create ? EAGAIN : __real_pthread_create(thread, attr, start, arg);
}

void *__wrap_calloc(size_t count, size_t size) {
  return fail_private ? NULL : __real_calloc(count, size);
}

/* Mapping reclamation is test-only and happens after all producers/workers
   joined. The real runtime retains shared pages until process disposal. */
static void destroy_node(struct fb_page_node *node, unsigned level) {
  if (!node) {
    return;
  }
  for (unsigned i = 0; i < 256; i++) {
    void *child = (void *)atomic_load(&node->child[i]);
    if (!child) {
      continue;
    }
    if (level) {
      destroy_node(child, level - 1);
    } else {
      assert(!munmap(child, sizeof(struct fb_context) * FB_PAGE_ITEMS));
    }
  }
  assert(!munmap(node, sizeof *node));
}

static void destroy_bridge(void) {
  assert(!atomic_load(&G->live_guest_threads));
  for (int i = 0; i < fb_context_capacity(G); i++) {
    /* The private guest_slot is only allocated by full_guest.c, and no
       producer/worker can still reference it here. */
    free((void *)atomic_load(&fb_context_at(G, i)->guest_private));
  }
  destroy_node((void *)atomic_load(&G->contexts.root), 3);
  free(G);
  G = NULL;
}

static void coalescing(void) {
  allocate_capacity(64);
  struct fb_context *a = fb_context_at(G, 1);
  struct fb_context *b = fb_context_at(G, 63);
  for (int n = 0; n < 100; n++) {
    fb_owner_post(G, 1);
  }
  fb_owner_post(G, 63);
  int batch = fb_owner_take(G);
  assert(batch == 63 && !fb_owner_take(G));
  int next = fb_owner_next(b);
  assert(next == 1);
  /* Requeue a detached node. Its overwritten next must not corrupt the
     saved batch; another post on the still-detached tail is coalesced. */
  fb_owner_post(G, 63);
  fb_owner_post(G, 1);
  assert(fb_owner_next(a) == 0);
  assert(fb_owner_take(G) == 63 && fb_owner_next(b) == 0);
  assert(!fb_owner_take(G));
  fb_context_reset(a);
  fb_owner_post(G, 1);
  fb_context_reset(a); /* Reset cannot erase pending membership. */
  assert(fb_owner_take(G) == 1 && fb_owner_next(a) == 0);
  assert(!fb_owner_take(G));
  destroy_bridge();
}

#define PRODUCERS 32
#define ROUNDS 2000
static _Atomic int acknowledged[PRODUCERS];
static pthread_barrier_t ready;

static void *producer(void *opaque) {
  int id = (int)(intptr_t)opaque;
  struct fb_context *c = fb_context_at(G, id);
  int rc = pthread_barrier_wait(&ready);
  assert(!rc || rc == PTHREAD_BARRIER_SERIAL_THREAD);
  for (int n = 1; n <= ROUNDS; n++) {
    /* Non-atomic payload deliberately tests publication, including
       coalescing before the consumer's acquire-clear. */
    c->host_tid = n;
    fb_owner_post(G, id);
    while (atomic_load_explicit(&acknowledged[id - 1], memory_order_acquire) !=
           n) {
      sched_yield();
    }
  }
  return NULL;
}

static void concurrent_posts(void) {
  allocate_capacity(64);
  assert(!pthread_barrier_init(&ready, NULL, PRODUCERS + 1));
  pthread_t threads[PRODUCERS];
  for (int i = 0; i < PRODUCERS; i++) {
    assert(!pthread_create(&threads[i], NULL, producer,
                           (void *)(intptr_t)(i + 1)));
  }
  int rc = pthread_barrier_wait(&ready);
  assert(!rc || rc == PTHREAD_BARRIER_SERIAL_THREAD);
  unsigned consumed = 0;
  uint64_t deadline = clock_ns(CLOCK_MONOTONIC) + 30000000000ULL;
  while (consumed < PRODUCERS * ROUNDS) {
    int batch = fb_owner_take(G);
    while (batch) {
      int id = batch;
      assert(id > 0 && id <= PRODUCERS);
      struct fb_context *c = fb_context_at(G, id);
      batch = fb_owner_next(c);
      int previous = atomic_load(&acknowledged[id - 1]);
      assert(c->host_tid == previous + 1);
      atomic_store_explicit(&acknowledged[id - 1], c->host_tid,
                            memory_order_release);
      consumed++;
    }
    assert(clock_ns(CLOCK_MONOTONIC) < deadline);
    sched_yield();
  }
  for (int i = 0; i < PRODUCERS; i++) {
    assert(!pthread_join(threads[i], NULL));
  }
  assert(!fb_owner_take(G));
  assert(!pthread_barrier_destroy(&ready));
  destroy_bridge();
}

static void *burst_producer(void *opaque) {
  int id = (int)(intptr_t)opaque;
  struct fb_context *c = fb_context_at(G, id);
  int rc = pthread_barrier_wait(&ready);
  assert(!rc || rc == PTHREAD_BARRIER_SERIAL_THREAD);
  for (int n = 1; n <= ROUNDS; n++) {
    atomic_store_explicit(&c->state, n, memory_order_release);
    fb_owner_post(G, id);
    if (n % 8 == 0) {
      sched_yield();
    }
  }
  return NULL;
}

static void coalesced_wakes(void) {
  allocate_capacity(64);
  assert(!pthread_barrier_init(&ready, NULL, PRODUCERS + 1));
  pthread_t threads[PRODUCERS];
  int seen[PRODUCERS] = {0};
  for (int i = 0; i < PRODUCERS; i++) {
    assert(!pthread_create(&threads[i], NULL, burst_producer,
                           (void *)(intptr_t)(i + 1)));
  }
  int rc = pthread_barrier_wait(&ready);
  assert(!rc || rc == PTHREAD_BARRIER_SERIAL_THREAD);
  int finished = 0;
  while (finished < PRODUCERS) {
    uint32_t observed = fb_event_sample(&G->ctx[0].guest_event);
    int batch = fb_owner_take(G);
    while (batch) {
      int id = batch;
      struct fb_context *c = fb_context_at(G, id);
      batch = fb_owner_next(c);
      int value = atomic_load_explicit(&c->state, memory_order_acquire);
      assert(value >= seen[id - 1] && value <= ROUNDS);
      finished += value == ROUNDS && seen[id - 1] != ROUNDS;
      seen[id - 1] = value;
    }
    if (finished < PRODUCERS) {
      fb_event_wait(&G->ctx[0].guest_event, observed);
    }
  }
  for (int i = 0; i < PRODUCERS; i++) {
    assert(!pthread_join(threads[i], NULL));
  }
  int batch = fb_owner_take(G);
  while (batch) {
    struct fb_context *c = fb_context_at(G, batch);
    batch = fb_owner_next(c);
    assert(atomic_load(&c->state) == ROUNDS);
  }
  assert(!pthread_barrier_destroy(&ready));
  destroy_bridge();
}

static void idle_high_water(void) {
  allocate_capacity(4096);
  for (int i = FB_CONTEXTS; i < 4096; i += FB_PAGE_ITEMS) {
    void *page = fb_pages_at(&G->contexts, (unsigned)(i - FB_CONTEXTS),
                             sizeof(struct fb_context), 0);
    assert(
        !mprotect(page, sizeof(struct fb_context) * FB_PAGE_ITEMS, PROT_NONE));
  }
  for (int i = 0; i < 1000; i++) {
    fg_pump(); /* Any historical-capacity scan now faults deterministically. */
  }
  for (int i = FB_CONTEXTS; i < 4096; i += FB_PAGE_ITEMS) {
    void *page = fb_pages_at(&G->contexts, (unsigned)(i - FB_CONTEXTS),
                             sizeof(struct fb_context), 0);
    assert(!mprotect(page, sizeof(struct fb_context) * FB_PAGE_ITEMS,
                     PROT_READ | PROT_WRITE));
  }
  destroy_bridge();
}

static void wait_for(int *ids, int count, int expected) {
  uint64_t deadline = clock_ns(CLOCK_MONOTONIC) + 10000000000ULL;
  for (;;) {
    fg_pump();
    int matched = 0;
    for (int i = 0; i < count; i++) {
      matched += atomic_load_explicit(&fb_context_at(G, ids[i])->state,
                                      memory_order_acquire) == expected;
    }
    if (matched == count) {
      return;
    }
    assert(clock_ns(CLOCK_MONOTONIC) < deadline);
    sched_yield();
  }
}

static void worker_lifecycle(void) {
  allocate_capacity(FB_CONTEXTS);
  for (int cycle = 0; cycle < 3; cycle++) {
    int ids[32];
    for (int i = 0; i < 32; i++) {
      ids[i] = fb_reserve_context(G);
      assert(ids[i] > 0);
      struct fb_context *c = fb_context_at(G, ids[i]);
      fb_context_reset(c);
      atomic_store_explicit(&c->state, FB_HOST_REQUEST, memory_order_release);
      fb_owner_post(G, ids[i]);
    }
    wait_for(ids, 32, FB_RUNNING);
    assert(atomic_load(&G->live_guest_threads) == 32);
    for (int i = 0; i < 32; i++) {
      struct fb_context *c = fb_context_at(G, ids[i]);
      atomic_store(&c->closing, 1);
      fb_event_notify(&c->guest_event);
    }
    wait_for(ids, 32, FB_JOINED);
    assert(!atomic_load(&G->live_guest_threads));
    for (int i = 0; i < 32; i++) {
      atomic_store(&fb_context_at(G, ids[i])->state, FB_FREE);
    }
  }
  assert(!fb_owner_take(G));
  destroy_bridge();
}

static void worker_failures(void) {
  allocate_capacity(FB_CONTEXTS);
  for (int fault = 0; fault < 2; fault++) {
    int id = fb_reserve_context(G);
    struct fb_context *c = fb_context_at(G, id);
    fb_context_reset(c);
    atomic_store(&c->state, FB_HOST_REQUEST);
    fail_private = fault == 0;
    fail_create = fault == 1;
    fb_owner_post(G, id);
    fg_pump();
    assert(atomic_load(&c->state) == FB_ATTACH_FAILED &&
           c->attach_result == JNI_ENOMEM);
    assert(!atomic_load(&c->owner_pending) && !fb_owner_take(G));
    fail_private = fail_create = 0;
    atomic_store(&c->state, FB_FREE);
    assert(fb_reserve_context(G) == id);
    fb_context_reset(c);
    atomic_store(&c->state, FB_HOST_REQUEST);
    fb_owner_post(G, id);
    wait_for(&id, 1, FB_RUNNING);
    atomic_store(&c->closing, 1);
    fb_event_notify(&c->guest_event);
    wait_for(&id, 1, FB_JOINED);
    atomic_store(&c->state, FB_FREE);
  }
  destroy_bridge();
}
#endif

int main(int argc, char **argv) {
  if (argc == 2) {
    benchmark(atoi(argv[1]));
    return 0;
  }
#ifndef GW_OWNER_BASELINE
  coalescing();
  concurrent_posts();
  coalesced_wakes();
  idle_high_water();
  worker_lifecycle();
  worker_failures();
  puts(
      "{\"result\":\"PASS\",\"concurrent_posts\":64000,"
      "\"coalesced_posts\":64000,"
      "\"worker_lifecycles\":98,\"coalescing\":true,\"requeue\":true,"
      "\"idle_protected_capacity\":4096,\"worker_faults\":2}");
#endif
}
