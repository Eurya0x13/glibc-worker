#include <string.h>
#include "binding_table.h"

/* Retained C control backend for the experiment. */
static struct binding bindings[512];

const char *fh_binding_backend(void) {
  return "c";
}

struct binding *binding_get(uint64_t module, uintptr_t guest, const char *sig,
                            int *created, int *status) {
  *created = 0;
  *status = GW_BINDING_OK;
  for (size_t i = 0; i < 512; i++) {
    struct binding *b = &bindings[i];
    if (b->used && b->module == module && b->guest == guest &&
        !strcmp(b->sig, sig)) {
      return b;
    }
  }
  for (size_t i = 0; i < 512; i++) {
    struct binding *b = &bindings[i];
    if (!b->used) {
      *status = fh_binding_prepare(b, module, guest, sig);
      if (*status != GW_BINDING_OK) {
        memset(b, 0, sizeof *b);
        return NULL;
      }
      b->used = 1;
      *created = 1;
      return b;
    }
  }
  *status = GW_BINDING_MEMORY;
  return NULL;
}

void binding_discard(struct binding *b) {
  if (fh_binding_retire(b)) {
    memset(b, 0, sizeof *b);
  }
}

void binding_release(uint64_t module) {
  for (size_t i = 0; i < 512; i++) {
    if (bindings[i].used && bindings[i].module == module) {
      binding_discard(&bindings[i]);
    }
  }
}

void binding_finish(void) {}
