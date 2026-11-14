#include "dlopen_backend.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Test the Host policy with an ordinary C adapter. This deliberately does not
 * claim to start a second libc; the JNI integration tests cover that runtime.
 */
static const struct gw_dlopen_api_v1 *api;
static int opens, closes, fail_next;

static int answer(int value) {
  return value + 7;
}

static int resolve(void *context, const char *path, char image[GW_LOADER_PATH],
                   struct gw_loader_error *error, void *unused) {
  (void)context;
  (void)error;
  (void)unused;
  snprintf(image, GW_LOADER_PATH, "%s", path);
  return GW_OK;
}

static int load(void *context, uint64_t id, const char *image,
                int32_t *metadata, struct gw_loader_error *error) {
  (void)context;
  (void)id;
  (void)image;
  opens++;

  if (fail_next) {
    fail_next = 0;
    return gw_dlopen_fail(error, GW_E_LOAD, "controlled C adapter failure");
  }

  *metadata = 71; /* An opaque C result, not a JNI version. */
  return GW_OK;
}

static int close_image(void *context, uint64_t id,
                       struct gw_loader_error *error) {
  (void)context;
  (void)id;
  (void)error;
  closes++;
  return GW_OK;
}

static int find(void *context, uint64_t id, const char *name,
                const char *binding, void **entry,
                struct gw_loader_error *error) {
  (void)context;
  (void)id;
  (void)binding;
  (void)error;
  if (strcmp(name, "answer")) {
    return GW_NOT_FOUND;
  }
  *entry = (void *)answer;
  return GW_OK;
}

static uint64_t current_module(void *context) {
  return context ? *(uint64_t *)context : 0;
}

static void module_capacity(void) {
  _Static_assert(FB_MODULES == 64, "reviewed module capacity");
  char directory[] = "/tmp/gw-module-capacity-XXXXXX";
  assert(mkdtemp(directory));
  char paths[FB_MODULES + 1][128];
  struct gw_dlopen_library libraries[FB_MODULES], extra;
  struct gw_loader_error error;
  for (size_t i = 0; i <= FB_MODULES; i++) {
    snprintf(paths[i], sizeof paths[i], "%s/%zu", directory, i);
    FILE *file = fopen(paths[i], "w");
    assert(file && !fclose(file));
    if (i < FB_MODULES) {
      assert(api->open(NULL, paths[i], GW_OPEN_GUEST, &libraries[i], &error) ==
             GW_OK);
      assert(fb_module_index(libraries[i].handle) == (int)i);
    }
  }
  int before = opens;
  assert(api->open(NULL, paths[FB_MODULES], GW_OPEN_GUEST, &extra, &error) ==
         GW_ERROR);
  assert(!extra.handle && error.code == GW_E_CAPACITY && opens == before);
  void *entry = NULL;
  assert(api->find(NULL, libraries[63].handle, "answer", NULL, &entry,
                   &error) == GW_OK &&
         ((int (*)(int))entry)(35) == 42);
  uint64_t retired = libraries[32].handle;
  assert(api->close(NULL, retired, &error) == GW_OK);
  assert(api->open(NULL, paths[FB_MODULES], GW_OPEN_GUEST, &libraries[32],
                   &error) == GW_OK);
  assert(libraries[32].handle != retired &&
         fb_module_index(libraries[32].handle) == 32);
  assert(api->owns_handle(retired) &&
         api->module_state(retired) == GW_MODULE_STALE);
  for (size_t i = 0; i < FB_MODULES; i++) {
    assert(api->close(NULL, libraries[i].handle, &error) == GW_OK);
  }
  for (size_t i = 0; i <= FB_MODULES; i++) {
    assert(!unlink(paths[i]));
  }
  assert(!rmdir(directory));
}

int main(void) {
  api = gw_dlopen_query(GW_DLOPEN_ABI_V1, sizeof *api);
  assert(api && !gw_dlopen_query(99, sizeof *api));
  assert(!gw_dlopen_query(GW_DLOPEN_ABI_V1, sizeof *api + 1));
  struct gw_loader_error error = {0};
  struct gw_dlopen_library lib = {0}, duplicate = {0};
  struct gw_dlopen_config config = {
      .transport = {load, close_image, find, current_module},
      .resolve_image = resolve};
  assert(gw_dlopen_configure(&config, &error) == GW_OK);

  char path[] = "/tmp/gw-loader-contract-XXXXXX";
  int fd = mkstemp(path);
  assert(fd >= 0);
  close(fd);

  assert(api->open(NULL, path, GW_OPEN_GUEST, &lib, &error) == GW_ERROR);
  assert(error.code == GW_E_NOT_READY);
  assert(gw_dlopen_activate(&error) == GW_OK);
  assert(api->open(NULL, path, GW_OPEN_AUTO, &lib, &error) == GW_ERROR);
  assert(error.code == GW_E_IMAGE && !lib.handle && opens == 0);
  assert(api->open(NULL, path, GW_OPEN_GUEST, &lib, &error) == GW_OK);
  assert(lib.metadata == 71 && api->owns_handle(lib.handle));
  assert(api->open(NULL, path, GW_OPEN_GUEST, &duplicate, &error) == GW_ERROR);
  assert(error.code == GW_E_DUPLICATE);

  void *entry = NULL;
  assert(api->find(NULL, lib.handle, "answer", NULL, &entry, &error) == GW_OK);
  assert(((int (*)(int))entry)(35) == 42);
  assert(api->find(NULL, lib.handle, "absent", NULL, &entry, &error) ==
         GW_NOT_FOUND);
  assert(!entry);

  assert(gw_dlopen_stop(&error) == GW_ERROR && error.code == GW_E_BUSY);
  assert(api->close(&lib.handle, lib.handle, &error) == GW_ERROR);
  assert(error.code == GW_E_BUSY);

  uint64_t old = lib.handle;
  assert(api->close(NULL, old, &error) == GW_OK);
  assert(api->owns_handle(old) && api->module_state(old) == GW_MODULE_STALE);

  fail_next = 1;
  assert(api->open(NULL, path, GW_OPEN_GUEST, &lib, &error) == GW_ERROR);
  assert(!lib.handle && error.code == GW_E_LOAD);
  assert(api->open(NULL, path, GW_OPEN_GUEST, &lib, &error) == GW_OK);
  assert(lib.handle != old);
  assert(api->find(NULL, old, "answer", NULL, &entry, &error) == GW_ERROR);
  assert(error.code == GW_E_HANDLE);
  assert(api->close(NULL, lib.handle, &error) == GW_OK);
  assert(opens == 3 && closes == 2);

  module_capacity();
  assert(gw_dlopen_stop(&error) == GW_OK);
  assert(api->close(NULL, 42, &error) == GW_PASS);
  unlink(path);
  puts("DLOPEN_C_CONTRACT result=PASS jni_required=0 opaque_metadata=71");
  return 0;
}
