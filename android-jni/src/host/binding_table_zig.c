#include <stdlib.h>
#include <string.h>
#include "binding_table.h"

static struct gw_binding_store *store;

const char *fh_binding_backend(void) {
  return "zig";
}

static void *allocate(void *context, size_t size, size_t alignment) {
  (void)context;
  void *memory = NULL;
  if (alignment < sizeof(void *)) {
    alignment = sizeof(void *);
  }
  return posix_memalign(&memory, alignment, size) ? NULL : memory;
}

static void deallocate(void *context, void *memory) {
  (void)context;
  free(memory);
}

static int prepare(void *context, void *object, uint64_t module,
                   uintptr_t guest, const char *signature) {
  (void)context;
  return fh_binding_prepare(object, module, guest, signature);
}

static int retire(void *context, void *object) {
  (void)context;
  return fh_binding_retire(object);
}

struct binding *binding_get(uint64_t module, uintptr_t guest, const char *sig,
                            int *created, int *status) {
  if (!store) {
    const struct gw_binding_callbacks callbacks = {
        .object_size = sizeof(struct binding),
        .object_alignment = _Alignof(struct binding),
        .signature_offset = offsetof(struct binding, sig),
        .signature_capacity = FB_SIG,
        .allocate = allocate,
        .deallocate = deallocate,
        .prepare = prepare,
        .retire = retire,
    };
    store = gw_binding_store_create(&callbacks);
  }
  if (!store) {
    *created = 0;
    *status = GW_BINDING_MEMORY;
    return NULL;
  }
  void *object = NULL;
  *status = gw_binding_store_get(store, module, guest, sig,
                                 strnlen(sig, FB_SIG), &object, created);
  return object;
}

void binding_discard(struct binding *b) {
  gw_binding_store_discard(store, b->module, b->guest, b->sig, strlen(b->sig));
}

void binding_release(uint64_t module) {
  if (store) {
    gw_binding_store_release(store, module);
  }
}

void binding_finish(void) {
  if (store && gw_binding_store_destroy(store) == GW_BINDING_OK) {
    store = NULL;
  }
}
