/* Terminal quarantine is tested in a fresh process for each failure phase. */
#include "dlopen_backend.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const struct gw_dlopen_api_v1 *api;
static uint64_t retained_id;
static int phase, loads, closes;
static struct gw_loader_error close_error;
static int close_result;

static int resolve(void *ctx, const char *path, char image[GW_LOADER_PATH],
                   struct gw_loader_error *error, void *unused) {
  (void)ctx;
  (void)error;
  (void)unused;

  snprintf(image, GW_LOADER_PATH, "%s", path);
  return GW_OK;
}

static int load(void *ctx, uint64_t id, const char *path, int32_t *metadata,
                struct gw_loader_error *error) {
  (void)ctx;
  (void)path;
  (void)metadata;

  retained_id = id;
  loads++;

  if (phase == 0) {
    gw_dlopen_quarantine();
    return gw_dlopen_fail(error, GW_E_QUARANTINED,
                          "Rollback failed; discard process");
  }
  return GW_OK;
}

static int close_image(void *ctx, uint64_t id, struct gw_loader_error *error) {
  (void)ctx;

  assert(id == retained_id);
  closes++;
  gw_dlopen_quarantine();
  return gw_dlopen_fail(error, GW_E_QUARANTINED,
                        "Close failed; discard process");
}

static int find(void *ctx, uint64_t id, const char *symbol, const char *sig,
                void **entry, struct gw_loader_error *error) {
  (void)ctx;
  (void)id;
  (void)symbol;
  (void)sig;
  (void)entry;
  (void)error;

  abort(); /* No lookup may reach the transport after quarantine. */
}

static uint64_t current(void *ctx) {
  (void)ctx;
  return 0;
}

static void *closer(void *ctx) {
  (void)ctx;
  close_result = api->close(NULL, retained_id, &close_error);
  return NULL;
}

int main(int argc, char **argv) {
  assert(argc == 2);
  phase = atoi(argv[1]);

  api = gw_dlopen_query(GW_DLOPEN_ABI_V1, sizeof *api);
  struct gw_dlopen_config config = {
      .transport = {load, close_image, find, current},
      .resolve_image = resolve};
  struct gw_loader_error error;
  assert(gw_dlopen_configure(&config, &error) == GW_OK);
  assert(gw_dlopen_activate(&error) == GW_OK);

  char path[] = "/tmp/gw-quarantine-XXXXXX";
  int fd = mkstemp(path);
  assert(fd >= 0);
  close(fd);

  struct gw_dlopen_library library;
  int opened = api->open(NULL, path, GW_OPEN_GUEST, &library, &error);
  if (phase == 0) {
    assert(opened == GW_ERROR && !library.handle &&
           error.code == GW_E_QUARANTINED);
    assert(api->module_state(retained_id) == GW_MODULE_LOADING);
  } else {
    assert(opened == GW_OK);
    if (phase == 1) {
      assert(api->close(NULL, retained_id, &error) == GW_ERROR);
      assert(closes == 1 && error.code == GW_E_QUARANTINED);
    } else {
      assert(phase == 3 ? gw_dlopen_scope_enter(retained_id)
                        : gw_dlopen_enter(retained_id));
      pthread_t thread;
      assert(!pthread_create(&thread, NULL, closer, NULL));
      while (api->module_state(retained_id) != GW_MODULE_CLOSING) {
        sched_yield();
      }

      /* Wake the closer without freeing its outstanding borrow. */
      gw_dlopen_quarantine();
      assert(!pthread_join(thread, NULL));
      assert(close_result == GW_ERROR && close_error.code == GW_E_QUARANTINED &&
             !closes);

      if (phase == 3) {
        assert(gw_dlopen_scope_leave(retained_id));
      } else {
        gw_dlopen_leave(retained_id);
      }
    }
    assert(api->module_state(retained_id) == GW_MODULE_CLOSING);
  }

  assert(gw_dlopen_is_quarantined() && !gw_dlopen_enter(retained_id));
  void *entry = (void *)(uintptr_t)1;
  assert(api->find(NULL, retained_id, "symbol", NULL, &entry, &error) ==
         GW_ERROR);
  assert(!entry && error.code == GW_E_QUARANTINED);
  int prior = closes;
  assert(api->close(NULL, retained_id, &error) == GW_ERROR && closes == prior);
  assert(api->open(NULL, path, GW_OPEN_GUEST, &library, &error) == GW_ERROR &&
         loads == 1);
  assert(gw_dlopen_stop(&error) == GW_ERROR && error.code == GW_E_QUARANTINED);
  assert(gw_dlopen_activate(&error) == GW_ERROR &&
         error.code == GW_E_QUARANTINED);
  assert(gw_dlopen_configure(&config, &error) == GW_ERROR &&
         error.code == GW_E_QUARANTINED);

  unlink(path);

  printf("QUARANTINE_TEST result=PASS phase=%d retained=1 no_process_exit=1\n",
         phase);
}
