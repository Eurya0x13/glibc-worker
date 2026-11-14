/* Production loader with a C transport: deterministic two/three-thread cycles.
 */
#define _GNU_SOURCE
#include "dlopen_backend.h"
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const struct gw_dlopen_api_v1 *api;
static unsigned checks, closes;
static uint64_t probe_scope;
static uint64_t probe_pin;

#define CHECK(expression)                                                     \
  do {                                                                        \
    checks++;                                                                 \
    if (!(expression)) {                                                      \
      fprintf(stderr, "Close wait check at %d: %s\n", __LINE__, #expression); \
      abort();                                                                \
    }                                                                         \
  } while (0)

static int resolve(void *ctx, const char *path, char image[GW_LOADER_PATH],
                   struct gw_loader_error *error, void *unused) {
  (void)ctx;
  (void)error;
  (void)unused;
  snprintf(image, GW_LOADER_PATH, "%s", path);
  return GW_OK;
}

static int load(void *ctx, uint64_t id, const char *image, int32_t *metadata,
                struct gw_loader_error *error) {
  (void)ctx;
  (void)id;
  (void)image;
  (void)error;
  *metadata = 0;
  return GW_OK;
}

static int close_image(void *ctx, uint64_t id, struct gw_loader_error *error) {
  (void)ctx;
  (void)id;
  (void)error;
  closes++;
  return GW_OK;
}

static int find(void *ctx, uint64_t id, const char *name, const char *sig,
                void **entry, struct gw_loader_error *error) {
  (void)ctx;
  (void)id;
  (void)name;
  (void)sig;
  (void)entry;
  (void)error;
  return GW_NOT_FOUND;
}

static uint64_t current(void *ctx) {
  (void)ctx;
  if (probe_pin) {
    uint64_t id = probe_pin;
    probe_pin = 0;
    if (!gw_dlopen_enter(id)) {
      abort();
    }
  }
  if (probe_scope) {
    uint64_t id = probe_scope;
    probe_scope = 0;
    if (!gw_dlopen_scope_enter(id)) {
      abort();
    }
  }
  return 0; /* No innermost-module shortcut can detect these cycles. */
}

struct cycle {
  pthread_barrier_t ready, returned;
  _Atomic unsigned completed;
};

struct participant {
  struct cycle *cycle;
  uint64_t own, target;
  int result, error, target_state;
};

static void meet(pthread_barrier_t *barrier) {
  int result = pthread_barrier_wait(barrier);
  if (result && result != PTHREAD_BARRIER_SERIAL_THREAD) {
    abort();
  }
}

static void *close_next(void *opaque) {
  struct participant *p = opaque;
  if (!gw_dlopen_scope_enter(p->own)) {
    abort();
  }
  meet(&p->cycle->ready);
  struct gw_loader_error error;
  p->result = api->close(NULL, p->target, &error);
  p->error = error.code;
  p->target_state = api->module_state(p->target);
  atomic_fetch_add(&p->cycle->completed, 1);
  /* Nobody releases its scope until every close has returned. */
  meet(&p->cycle->returned);
  if (!gw_dlopen_scope_leave(p->own)) {
    abort();
  }
  return NULL;
}

static void cycle(const struct gw_dlopen_library *modules, unsigned count) {
  struct cycle shared = {0};
  struct participant p[3] = {0};
  pthread_t threads[3];
  unsigned before = closes;
  CHECK(!pthread_barrier_init(&shared.ready, NULL, count + 1));
  CHECK(!pthread_barrier_init(&shared.returned, NULL, count));
  for (unsigned i = 0; i < count; i++) {
    p[i].cycle = &shared;
    p[i].own = modules[i].handle;
    p[i].target = modules[(i + 1) % count].handle;
    CHECK(!pthread_create(&threads[i], NULL, close_next, &p[i]));
  }
  meet(&shared.ready);
  while (atomic_load(&shared.completed) != count) {
    unsigned closing = 0;
    for (unsigned i = 0; i < count; i++) {
      closing += api->module_state(modules[i].handle) == GW_MODULE_CLOSING;
    }
    if (closing == count) {
      fprintf(stderr,
              "{\"result\":\"FAIL\",\"reason\":\"cyclic_close_wait\","
              "\"threads\":%u,\"closing\":%u}\n",
              count, closing);
      abort();
    }
    sched_yield();
  }
  for (unsigned i = 0; i < count; i++) {
    CHECK(!pthread_join(threads[i], NULL));
    CHECK(p[i].result == GW_ERROR && p[i].error == GW_E_BUSY);
    CHECK(p[i].target_state == GW_MODULE_LIVE);
  }
  CHECK(closes == before && !gw_dlopen_is_quarantined());
  CHECK(!pthread_barrier_destroy(&shared.ready));
  CHECK(!pthread_barrier_destroy(&shared.returned));
}

static void pinned_target(uint64_t owner, uint64_t target) {
  struct gw_loader_error error;
  unsigned before = closes;
  CHECK(gw_dlopen_enter(target));
  CHECK(gw_dlopen_scope_enter(owner));
  CHECK(api->close(NULL, target, &error) == GW_ERROR &&
        error.code == GW_E_BUSY);
  CHECK(api->module_state(target) == GW_MODULE_LIVE && closes == before);
  CHECK(gw_dlopen_scope_leave(owner));
  /* Probe changes the caller's scopes after close's first validation. */
  probe_scope = owner;
  CHECK(api->close(NULL, target, &error) == GW_ERROR &&
        error.code == GW_E_BUSY);
  CHECK(api->module_state(target) == GW_MODULE_LIVE && closes == before);
  CHECK(gw_dlopen_scope_leave(owner));
  gw_dlopen_leave(target);
  /* The target can also gain a borrower while the lock is released. */
  CHECK(gw_dlopen_scope_enter(owner));
  probe_pin = target;
  CHECK(api->close(NULL, target, &error) == GW_ERROR &&
        error.code == GW_E_BUSY);
  CHECK(api->module_state(target) == GW_MODULE_LIVE && closes == before);
  CHECK(gw_dlopen_scope_leave(owner));
  gw_dlopen_leave(target);
}

int main(void) {
  api = gw_dlopen_query(GW_DLOPEN_ABI_V1, sizeof *api);
  CHECK(api);
  struct gw_dlopen_config config = {
      .transport = {load, close_image, find, current},
      .resolve_image = resolve};
  struct gw_loader_error error;
  CHECK(gw_dlopen_configure(&config, &error) == GW_OK);
  CHECK(gw_dlopen_activate(&error) == GW_OK);
  char directory[] = "/tmp/gw-close-waits-XXXXXX";
  CHECK(mkdtemp(directory));
  char paths[3][128];
  struct gw_dlopen_library modules[3];
  for (unsigned i = 0; i < 3; i++) {
    snprintf(paths[i], sizeof paths[i], "%s/%u", directory, i);
    FILE *file = fopen(paths[i], "w");
    CHECK(file && !fclose(file));
    CHECK(api->open(NULL, paths[i], GW_OPEN_GUEST, &modules[i], &error) ==
          GW_OK);
  }
  cycle(modules, 2);
  cycle(modules, 3);
  pinned_target(modules[0].handle, modules[1].handle);
  CHECK(gw_dlopen_scope_enter(modules[0].handle));
  CHECK(api->close(NULL, modules[1].handle, &error) == GW_OK); /* No wait. */
  CHECK(gw_dlopen_scope_leave(modules[0].handle));
  CHECK(api->close(NULL, modules[0].handle, &error) == GW_OK);
  CHECK(api->close(NULL, modules[2].handle, &error) == GW_OK);
  CHECK(closes == 3);
  for (unsigned i = 0; i < 3; i++) {
    CHECK(!unlink(paths[i]));
  }
  CHECK(!rmdir(directory));
  CHECK(gw_dlopen_stop(&error) == GW_OK);
  printf(
      "{\"result\":\"PASS\",\"checks\":%u,\"cycle_sizes\":[2,3],"
      "\"pin_wait_refused\":true,\"probe_rechecked\":true,"
      "\"idle_close_allowed\":true}\n",
      checks);
}
