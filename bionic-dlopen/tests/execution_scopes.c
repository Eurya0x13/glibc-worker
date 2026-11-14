/* Production loader, plain C transport. No VM or second libc is simulated. */
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
static uint64_t enter_during_probe;
static int reject_close;

#define CHECK(expression)                                         \
  do {                                                            \
    checks++;                                                     \
    if (!(expression)) {                                          \
      fprintf(stderr, "Scope check failed at %d: %s\n", __LINE__, \
              #expression);                                       \
      abort();                                                    \
    }                                                             \
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

  *metadata = 71;
  return GW_OK;
}

static int close_image(void *ctx, uint64_t id, struct gw_loader_error *error) {
  (void)ctx;
  (void)id;

  closes++;
  return reject_close
             ? gw_dlopen_fail(error, GW_E_LOAD, "controlled close refusal")
             : GW_OK;
}

static uint64_t current(void *ctx) {
  if (enter_during_probe) {
    uint64_t id = enter_during_probe;
    enter_during_probe = 0;
    if (!gw_dlopen_scope_enter(id)) {
      abort();
    }
  }

  return ctx ? *(uint64_t *)ctx : 0;
}

static int find(void *ctx, uint64_t id, const char *name, const char *sig,
                void **entry, struct gw_loader_error *error) {
  (void)ctx;
  (void)name;
  (void)sig;
  (void)entry;

  unsigned before = closes;
  if (api->close(NULL, id, error) != GW_ERROR || error->code != GW_E_BUSY ||
      api->module_state(id) != GW_MODULE_LIVE || closes != before) {
    abort();
  }

  return GW_NOT_FOUND;
}

static void busy(uint64_t target, uint64_t innermost) {
  struct gw_loader_error error;
  unsigned before = closes;

  CHECK(api->close(&innermost, target, &error) == GW_ERROR);
  CHECK(error.code == GW_E_BUSY && api->module_state(target) == GW_MODULE_LIVE);
  CHECK(closes == before && !gw_dlopen_is_quarantined());
}

static void nested(uint64_t a, uint64_t b) {
  CHECK(gw_dlopen_scope_enter(a));
  busy(a, a); /* Direct self close. */
  CHECK(gw_dlopen_scope_enter(b));
  busy(a, b); /* A -> B -> close(A). */
  CHECK(gw_dlopen_scope_enter(a));
  busy(a, b); /* Repeated occurrence of A. */
  CHECK(gw_dlopen_scope_leave(a));
  busy(a, b); /* Outer A remains protected. */
  CHECK(gw_dlopen_scope_leave(b));
  busy(a, 0);

  /* A general leave must not consume an executing scope. */
  gw_dlopen_leave(a);
  busy(a, 0);
  CHECK(gw_dlopen_scope_leave(a));
  CHECK(!gw_dlopen_scope_leave(a));
  CHECK(!gw_dlopen_scope_leave(42));

  /* Repetition has no independent 12/512-depth storage ceiling. */
  for (unsigned i = 0; i < 1024; i++) {
    CHECK(gw_dlopen_scope_enter(a));
  }
  for (unsigned i = 0; i < 1023; i++) {
    CHECK(gw_dlopen_scope_leave(a));
  }
  busy(a, 0);
  CHECK(gw_dlopen_scope_leave(a));
}

struct closing {
  uint64_t id;
  _Atomic int done, foreign_leave;
  int result;
};

static void *closer(void *opaque) {
  struct closing *test = opaque;
  struct gw_loader_error error;

  atomic_store(&test->foreign_leave, gw_dlopen_scope_leave(test->id));
  test->result = api->close(NULL, test->id, &error);
  atomic_store(&test->done, 1);
  return NULL;
}

static void cross_thread(uint64_t id) {
  CHECK(gw_dlopen_scope_enter(id));
  struct closing test = {.id = id, .foreign_leave = -1};
  pthread_t thread;
  CHECK(!pthread_create(&thread, NULL, closer, &test));

  while (api->module_state(id) != GW_MODULE_CLOSING) {
    sched_yield();
  }

  CHECK(!atomic_load(&test.foreign_leave) && !atomic_load(&test.done));
  CHECK(!gw_dlopen_scope_enter(id) && !gw_dlopen_enter(id));
  gw_dlopen_leave(id); /* Cannot steal the scope during close. */
  CHECK(!atomic_load(&test.done));

  CHECK(gw_dlopen_scope_leave(id));
  CHECK(!pthread_join(thread, NULL));
  CHECK(test.result == GW_OK && atomic_load(&test.done));
  CHECK(!gw_dlopen_scope_leave(id) && api->module_state(id) == GW_MODULE_STALE);
}

static void *unpin(void *opaque) {
  uint64_t id = *(uint64_t *)opaque;

  while (api->module_state(id) != GW_MODULE_CLOSING) {
    sched_yield();
  }

  gw_dlopen_leave(id);
  return NULL;
}

static void transferable(uint64_t id) {
  CHECK(gw_dlopen_enter(id));
  CHECK(!gw_dlopen_scope_leave(id)); /* Cannot consume a general pin either. */

  pthread_t thread;
  CHECK(!pthread_create(&thread, NULL, unpin, &id));

  struct gw_loader_error error;
  CHECK(api->close(NULL, id, &error) == GW_OK);
  CHECK(!pthread_join(thread, NULL));
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

  char directory[] = "/tmp/gw-scopes-XXXXXX";
  CHECK(mkdtemp(directory));
  char paths[FB_MODULES][128];
  struct gw_dlopen_library modules[FB_MODULES];
  for (unsigned i = 0; i < FB_MODULES; i++) {
    snprintf(paths[i], sizeof paths[i], "%s/%u", directory, i);
    FILE *file = fopen(paths[i], "w");
    CHECK(file && !fclose(file));
    CHECK(api->open(NULL, paths[i], GW_OPEN_GUEST, &modules[i], &error) ==
          GW_OK);
    CHECK(gw_dlopen_scope_enter(modules[i].handle));
  }

  busy(modules[0].handle, modules[63].handle);
  for (unsigned i = 0; i < FB_MODULES; i++) {
    CHECK(gw_dlopen_scope_leave(modules[i].handle));
  }

  uint64_t a = modules[0].handle, b = modules[1].handle;
  nested(a, b);

  void *entry = NULL;
  CHECK(api->find(NULL, a, "scope", NULL, &entry, &error) == GW_NOT_FOUND);
  CHECK(!gw_dlopen_scope_leave(a)); /* find's automatic scope was released. */

  enter_during_probe = a;
  busy(a, b); /* Recheck after the transport probe. */
  CHECK(gw_dlopen_scope_leave(a));

  reject_close = 1;
  CHECK(api->close(NULL, a, &error) == GW_ERROR && error.code == GW_E_LOAD);
  CHECK(api->module_state(a) == GW_MODULE_LIVE);
  reject_close = 0;

  cross_thread(a);

  CHECK(api->open(NULL, paths[0], GW_OPEN_GUEST, &modules[0], &error) == GW_OK);
  CHECK(modules[0].handle != a && !gw_dlopen_scope_enter(a));
  CHECK(gw_dlopen_scope_enter(modules[0].handle));
  busy(modules[0].handle, 0);
  CHECK(gw_dlopen_scope_leave(modules[0].handle));

  transferable(b);

  for (unsigned i = 0; i < FB_MODULES; i++) {
    if (i != 1) {
      CHECK(api->close(NULL, modules[i].handle, &error) == GW_OK);
    }
    CHECK(!unlink(paths[i]));
  }
  CHECK(!rmdir(directory));
  CHECK(gw_dlopen_stop(&error) == GW_OK);

  printf(
      "{\"result\":\"PASS\",\"checks\":%u,\"ancestor_close\":true,"
      "\"recursive_scopes\":true,\"thread_affinity\":true,"
      "\"transferable_pins\":true,\"cross_thread_close\":true}\n",
      checks);
}
