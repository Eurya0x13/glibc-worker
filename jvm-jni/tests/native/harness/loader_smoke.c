#include "loader_smoke.h"
#include "loader_backend_internal.h"
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

/* Test-app client of the same public ABI intended for JRE source integration.
   No NativeLibraries field access, Java symbol decoding, or reflection. */
static int checks, failures;

struct active_borrow {
  const struct gw_loader_api_v1 *api;
  uint64_t handle;
  _Atomic int closing_observed, released;
};

static void *release_borrow(void *opaque) {
  struct active_borrow *borrow = opaque;
  for (int i = 0; i < 1000; i++) {
    if (borrow->api->module_state(borrow->handle) == GW_MODULE_CLOSING) {
      atomic_store(&borrow->closing_observed, 1);
      break;
    }
    usleep(100);
  }

  atomic_store_explicit(&borrow->released, 1, memory_order_release);
  gw_loader_leave(borrow->handle);
  return NULL;
}

static void check(int value, const char *name) {
  checks++;
  if (!value) {
    failures++;
  }
  printf("LOADER_BACKEND_TEST %s=%s\n", name, value ? "PASS" : "FAIL");
}

int gw_loader_smoke_not_ready(JNIEnv *env, const char *directory) {
  const struct gw_loader_api_v1 *api =
      gw_loader_query(GW_LOADER_ABI_V1, sizeof *api);
  check(api && api->abi_version == GW_LOADER_ABI_V1 &&
            api->struct_size >= sizeof *api,
        "versioned_abi_available");
  check(!gw_loader_query(999, sizeof *api), "unknown_abi_rejected");
  check(!gw_loader_query(GW_LOADER_ABI_V1, sizeof *api + 1),
        "oversized_api_rejected");
  if (!api) {
    return 1;
  }

  char path[GW_LOADER_PATH];
  snprintf(path, sizeof path, "%s/libinstalled_zstd.so", directory);
  struct gw_loader_error error = {0};
  struct gw_loader_library lib = {0};
  int rc = api->open(env, path, GW_OPEN_AUTO, &lib, &error);
  check(rc == GW_ERROR && error.code == GW_E_NOT_READY && !lib.handle,
        "guest_load_before_ready_rejected");
  return failures;
}

int gw_loader_smoke(JNIEnv *env, const char *directory) {
  const struct gw_loader_api_v1 *api =
      gw_loader_query(GW_LOADER_ABI_V1, sizeof *api);
  struct gw_loader_error error = {0};
  struct gw_loader_library lib = {0};
  void *entry = NULL;
  char path[GW_LOADER_PATH];
  snprintf(path, sizeof path, "%s/libhost_control.so", directory);

  check(api->open(env, path, GW_OPEN_AUTO, &lib, &error) == GW_PASS &&
            !lib.handle,
        "bionic_open_passthrough");
  check(api->find(env, 42, "anything", "()V", &entry, &error) == GW_PASS &&
            !entry,
        "bionic_find_passthrough");
  check(api->close(env, 42, &error) == GW_PASS, "bionic_close_passthrough");
  check(api->open(env, path, 2, &lib, &error) == GW_ERROR &&
            error.code == GW_E_ARGUMENT,
        "unknown_open_flags_rejected");

  snprintf(path, sizeof path, "%s/libinstalled_zstd.so", directory);
  int rc = api->open(env, path, GW_OPEN_AUTO, &lib, &error);
  check(rc == GW_OK && lib.handle, "direct_backend_open_original_zstd");
  if (rc != GW_OK) {
    goto done;
  }

  check(lib.jni_version == JNI_VERSION_1_1, "absent_onload_version_is_1_1");
  check(api->find(env, lib.handle, "not_a_real_zstd_symbol", "()V", &entry,
                  &error) == GW_NOT_FOUND &&
            !entry,
        "missing_symbol_is_not_found");
  for (const char **bad =
           (const char *[]){"(V)J", "(J)Jtrailing", "([V)J", "(J)L;", NULL};
       *bad; bad++) {
    rc = api->find(env, lib.handle,
                   "Java_com_github_luben_zstd_Zstd_compressBound", *bad,
                   &entry, &error);
    check(rc == GW_ERROR && error.code == GW_E_ARGUMENT && !entry, *bad);
  }

  const char *symbol = "Java_com_github_luben_zstd_Zstd_compressBound";
  rc = api->find(env, lib.handle, symbol, "(J)J", &entry, &error);
  check(rc == GW_OK && entry, "explicit_descriptor_returns_host_trampoline");
  if (rc == GW_OK) {
    void *again = NULL;
    check(api->find(env, lib.handle, symbol, "(J)J", &again, &error) == GW_OK &&
              entry == again,
          "repeated_lookup_reuses_binding");
    jlong (*call)(JNIEnv *, jclass, jlong) = (void *)entry;
    check(call(env, NULL, 1024) > 1024 && !(*env)->ExceptionCheck(env),
          "explicit_backend_native_call");
  }

  check(gw_loader_stop(&error) == GW_ERROR && error.code == GW_E_BUSY,
        "stop_with_live_image_refused");
  check(api->module_state(lib.handle) == GW_MODULE_LIVE,
        "refused_stop_leaves_backend_running");

  /* Hold an actual backend borrow across close: it must wait for leave(). */
  struct active_borrow borrow = {.api = api, .handle = lib.handle};
  pthread_t releaser;
  int entered = gw_loader_enter(lib.handle);
  int started =
      entered && !pthread_create(&releaser, NULL, release_borrow, &borrow);
  if (entered && !started) {
    gw_loader_leave(lib.handle);
  }
  int closed = api->close(env, lib.handle, &error);
  if (started) {
    pthread_join(releaser, NULL);
  }
  check(closed == GW_OK && started && atomic_load(&borrow.closing_observed) &&
            atomic_load(&borrow.released),
        "explicit_backend_close_waits_for_active_borrow");
  check(api->owns_handle(lib.handle),
        "retired_handle_still_recognized_as_guest");
  check(api->module_state(lib.handle) == GW_MODULE_STALE,
        "closed_handle_is_stale");
  check(
      api->find(env, lib.handle, symbol, "(J)J", &entry, &error) == GW_ERROR &&
          error.code == GW_E_HANDLE && !entry,
      "stale_find_never_falls_back");
  check(api->close(env, lib.handle, &error) == GW_ERROR &&
            error.code == GW_E_HANDLE,
        "stale_close_never_falls_back");

done:
  check(!(*env)->ExceptionCheck(env), "contract_has_no_pending_exception");
  printf("LOADER_BACKEND_TEST_SUMMARY checks=%d failures=%d abi=%u\n", checks,
         failures, GW_LOADER_ABI_V1);
  return failures;
}
