/* Shared POD pages and reservation races, independent of any VM/libc bridge. */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include "full_protocol.h"

static _Atomic long mapping_calls, fail_mapping = -1;
static unsigned checks;
#define CHECK(x) \
  do {           \
    assert(x);   \
    checks++;    \
  } while (0)
void *__real_mmap(void *, size_t, int, int, int, off_t);

void *__wrap_mmap(void *p, size_t n, int prot, int flags, int fd, off_t off) {
  long call = atomic_fetch_add(&mapping_calls, 1);
  if (call == atomic_load(&fail_mapping)) {
    errno = ENOMEM;
    return MAP_FAILED;
  }
  return __real_mmap(p, n, prot, flags, fd, off);
}

/* Test-only reclamation, after every user has joined. Production retains pages.
 */
static void destroy_node(struct fb_page_node *node, unsigned level,
                         size_t stride) {
  if (!node) {
    return;
  }
  for (unsigned i = 0; i < 256; i++) {
    void *child = (void *)atomic_load(&node->child[i]);
    if (!child) {
      continue;
    }
    if (level) {
      destroy_node(child, level - 1, stride);
    } else {
      assert(!munmap(child, stride * FB_PAGE_ITEMS));
    }
  }
  assert(!munmap(node, sizeof *node));
}

static void destroy_bridge(struct full_bridge *b) {
  for (int i = 0; i < fb_context_capacity(b); i++) {
    struct fb_context *c = fb_context_at(b, i);
    destroy_node((void *)atomic_load(&c->frames.root), 3,
                 sizeof(struct fb_frame));
  }
  destroy_node((void *)atomic_load(&b->contexts.root), 3,
               sizeof(struct fb_context));
  free(b);
}

struct job {
  struct full_bridge *bridge;
  pthread_barrier_t *ready;
  int id;
};

static void *reserve(void *opaque) {
  struct job *job = opaque;
  job->id = fb_reserve_context(job->bridge);
  assert(job->id > 0);
  struct fb_context *c = fb_context_at(job->bridge, job->id);
  fb_context_reset(c);
  struct fb_call *first = fb_call_at(c, 0);
  first->args[0].i = job->id;
  assert(fb_ensure_frame(c, FB_DEPTH));
  struct fb_rpc *early = fb_rpc_at(c, FB_DEPTH);
  early->result.i = job->id;
  for (int depth = 0; depth < 96; depth++) {
    assert(fb_ensure_frame(c, depth));
  }
  assert(fb_call_at(c, 0) == first && first->args[0].i == job->id);
  assert(fb_rpc_at(c, FB_DEPTH) == early && early->result.i == job->id);
  int result = pthread_barrier_wait(job->ready);
  assert(!result || result == PTHREAD_BARRIER_SERIAL_THREAD);
  return NULL; /* Keep the reservations; no other thread can claim these IDs. */
}

static void concurrent_growth(void) {
  struct full_bridge *b = calloc(1, sizeof *b);
  CHECK(b);
  pthread_barrier_t ready;
  CHECK(!pthread_barrier_init(&ready, NULL, 48));
  pthread_t threads[48];
  struct job jobs[48];
  for (unsigned i = 0; i < 48; i++) {
    jobs[i] = (struct job){.bridge = b, .ready = &ready};
    CHECK(!pthread_create(&threads[i], NULL, reserve, &jobs[i]));
  }
  for (unsigned i = 0; i < 48; i++) {
    CHECK(!pthread_join(threads[i], NULL));
  }
  CHECK(!pthread_barrier_destroy(&ready));
  CHECK(fb_context_capacity(b) > 48);
  for (unsigned i = 0; i < 48; i++) {
    for (unsigned j = 0; j < i; j++) {
      CHECK(jobs[i].id != jobs[j].id);
    }
    CHECK(fb_frame_capacity(fb_context_at(b, jobs[i].id)) >= 96);
  }
  int before = fb_context_capacity(b);
  for (int i = 1; i < before; i++) {
    atomic_store(&fb_context_at(b, i)->state, FB_RESERVED);
  }
  atomic_store(&fail_mapping, atomic_load(&mapping_calls));
  CHECK(fb_reserve_context(b) == -1 && fb_context_capacity(b) == before);
  atomic_store(&fail_mapping, -1);
  CHECK(fb_reserve_context(b) == before &&
        fb_context_capacity(b) == before + FB_PAGE_ITEMS);
  destroy_bridge(b);
}

static void growth_failures(void) {
  for (unsigned fault = 0; fault < 5; fault++) {
    struct full_bridge *b = calloc(1, sizeof *b);
    CHECK(b);
    struct fb_context *c = fb_context_at(b, 0);
    struct fb_call *first = fb_call_at(c, 0);
    first->args[0].i = 91;
    atomic_store(&fail_mapping, atomic_load(&mapping_calls) + fault);
    CHECK(!fb_ensure_frame(c, FB_DEPTH));
    CHECK(fb_frame_capacity(c) == FB_DEPTH && fb_call_at(c, FB_DEPTH) == NULL);
    CHECK(fb_call_at(c, 0) == first && first->args[0].i == 91);
    atomic_store(&fail_mapping, -1);
    CHECK(fb_ensure_frame(c, FB_DEPTH) && fb_rpc_at(c, FB_DEPTH));
    CHECK(!fb_ensure_frame(c, INT_MAX) && !fb_ensure_frame(c, -1));
    destroy_bridge(b);
  }
}

int main(void) {
  growth_failures();
  concurrent_growth();
  printf(
      "{\"result\":\"PASS\",\"checks\":%u,\"threads\":48,\"depth\":96,"
      "\"mapping_faults\":5,\"stable_addresses\":true,\"unique_contexts\":true}"
      "\n",
      checks);
}
