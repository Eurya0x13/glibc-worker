#include "loader_backend_internal.h"
#include "dlopen_backend.h"
#include "jni_capabilities.h"
#include <stdio.h>
#include <string.h>

static struct gw_loader_config config;

int gw_loader_fail(struct gw_loader_error *error, int code, const char *text) {
  return gw_dlopen_fail(error, code, text);
}

int gw_loader_version_supported(jint v) {
  return gw_jni_accepts(GW_JNI_MAX_VERSION, v);
}

static int load(void *context, uint64_t id, const char *path, int32_t *metadata,
                struct gw_loader_error *error) {
  jint version = config.transport.load(context, id, path, error);
  *metadata = version;
  return gw_loader_version_supported(version) ? GW_OK : GW_ERROR;
}

static int close_image(void *context, uint64_t id,
                       struct gw_loader_error *error) {
  return config.transport.close(context, id, error);
}

static int find(void *context, uint64_t id, const char *symbol,
                const char *binding, void **entry,
                struct gw_loader_error *error) {
  return config.transport.find(context, id, symbol, binding, entry, error);
}

static uint64_t current_module(void *context) {
  return config.transport.current_module(context);
}

static int resolve_image(void *context, const char *path,
                         char image[GW_LOADER_PATH],
                         struct gw_loader_error *error, void *unused) {
  (void)unused;
  return config.resolve_image(context, path, image, error,
                              config.image_context);
}

static void observe(int event, uint64_t id, int32_t metadata, void *unused) {
  (void)id;
  (void)metadata;
  (void)unused;
  struct full_bridge *counters = config.counters;

  if (event == GW_DLOPEN_OPENED) {
    int active = atomic_fetch_add(&counters->active_modules, 1) + 1;
    int peak = atomic_load(&counters->peak_modules);
    while (peak < active && !atomic_compare_exchange_weak(
                                &counters->peak_modules, &peak, active)) {
    }
  } else if (event == GW_DLOPEN_CLOSED) {
    atomic_fetch_sub(&counters->active_modules, 1);
    atomic_fetch_add(&counters->unloads, 1);
  } else {
    atomic_fetch_add(&counters->failed_loads, 1);
  }
}

int gw_loader_configure(const struct gw_loader_config *value,
                        struct gw_loader_error *error) {
  if (!value || !value->counters || !value->resolve_image ||
      !value->transport.load || !value->transport.find ||
      !value->transport.close || !value->transport.current_module) {
    return gw_loader_fail(error, GW_E_ARGUMENT,
                          "Incomplete JNI loader configuration");
  }

  struct gw_dlopen_config base = {
      .transport = {.load = load,
                    .close = close_image,
                    .find = find,
                    .current_module = current_module},
      .resolve_image = resolve_image,
      .observe = observe};

  int rc = gw_dlopen_configure(&base, error);
  if (rc == GW_OK) {
    config = *value;
  }
  return rc;
}

int gw_loader_activate(struct gw_loader_error *error) {
  return gw_dlopen_activate(error);
}

int gw_loader_stop(struct gw_loader_error *error) {
  return gw_dlopen_stop(error);
}

int gw_loader_enter(uint64_t id) {
  return gw_dlopen_enter(id);
}

void gw_loader_leave(uint64_t id) {
  gw_dlopen_leave(id);
}

int gw_loader_scope_enter(uint64_t id) {
  return gw_dlopen_scope_enter(id);
}

int gw_loader_scope_leave(uint64_t id) {
  return gw_dlopen_scope_leave(id);
}

uint64_t gw_loader_last_handle(void) {
  return gw_dlopen_last_handle();
}

/* A full descriptor is required: short JNI export names do not encode types. */
static int type(const char **cursor, int allow_void) {
  const char *s = *cursor;
  if (*s == 'V' && allow_void) {
    *cursor = s + 1;
    return 1;
  }

  if (*s && strchr("ZBCSIJFD", *s)) {
    *cursor = s + 1;
    return 1;
  }

  if (*s == '[') {
    do {
      s++;
    } while (*s == '[');
    if (!type(&s, 0)) {
      return 0;
    }
    *cursor = s;
    return 1;
  }

  if (*s == 'L') {
    const char *start = ++s;
    while (*s && *s != ';') {
      if (*s == '.' || *s == '[') {
        return 0;
      }
      s++;
    }
    if (s == start || *s != ';') {
      return 0;
    }
    *cursor = s + 1;
    return 1;
  }
  return 0;
}

static int valid_descriptor(const char *s) {
  if (!s || strlen(s) >= GW_LOADER_SIGNATURE || *s++ != '(') {
    return 0;
  }

  int count = 0;
  while (*s && *s != ')') {
    if (++count > FB_ARGS || !type(&s, 0)) {
      return 0;
    }
  }

  return *s++ == ')' && type(&s, 1) && !*s;
}

static const struct gw_dlopen_api_v1 *base_api(void) {
  return gw_dlopen_query(GW_DLOPEN_ABI_V1, sizeof(struct gw_dlopen_api_v1));
}

static int open_image(JNIEnv *env, const char *path, uint32_t flags,
                      struct gw_loader_library *out,
                      struct gw_loader_error *error) {
  if (out) {
    memset(out, 0, sizeof *out);
  }
  if (!env || !out) {
    return gw_loader_fail(error, GW_E_ARGUMENT,
                          "JNI environment and output required");
  }

  struct gw_dlopen_library library = {0};
  int rc = base_api()->open(env, path, flags, &library, error);
  out->handle = library.handle;
  out->jni_version = library.metadata;
  return rc;
}

static int find_entry(JNIEnv *env, uint64_t id, const char *symbol,
                      const char *descriptor, void **entry,
                      struct gw_loader_error *error) {
  if (entry) {
    *entry = NULL;
  }
  if (base_api()->owns_handle(id) && (!env || !valid_descriptor(descriptor))) {
    return gw_loader_fail(error, GW_E_ARGUMENT,
                          "Explicit, valid JNI method descriptor required");
  }
  return base_api()->find(env, id, symbol, descriptor, entry, error);
}

static int close_library(JNIEnv *env, uint64_t id,
                         struct gw_loader_error *error) {
  if (base_api()->owns_handle(id) && !env) {
    return gw_loader_fail(error, GW_E_ARGUMENT, "JNI environment required");
  }

  int rc = base_api()->close(env, id, error);
  if (rc == GW_OK) {
    printf(
        "LOAD_PROXY operation=unload target=glibc handle=%llx unloads=%d "
        "destructors=%d\n",
        (unsigned long long)id, atomic_load(&config.counters->unloads),
        atomic_load(&config.counters->destructors));
  }
  return rc;
}

static int owns_handle(uint64_t id) {
  return base_api()->owns_handle(id);
}

static int module_state(uint64_t id) {
  return base_api()->module_state(id);
}

static const struct gw_loader_api_v1 api = {
    GW_LOADER_ABI_V1, sizeof api,  open_image,  find_entry,
    close_library,    owns_handle, module_state};

const struct gw_loader_api_v1 *gw_loader_query(uint32_t version,
                                               size_t minimum_size) {
  return version == GW_LOADER_ABI_V1 && minimum_size <= sizeof api ? &api
                                                                   : NULL;
}
