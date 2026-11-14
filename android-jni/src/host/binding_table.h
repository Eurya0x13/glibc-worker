#ifndef GW_BINDING_TABLE_H
#define GW_BINDING_TABLE_H
#include "host_internal.h"
#include "binding_store.h"
#pragma GCC visibility push(hidden)

/* Called under bindings_lock. discard is only for newly created bindings
   before their addresses are exposed to the VM or another lookup caller. */
struct binding *binding_get(uint64_t, uintptr_t, const char *, int *, int *);
void binding_discard(struct binding *);
void binding_release(uint64_t);
void binding_finish(void);
int fh_binding_prepare(struct binding *, uint64_t, uintptr_t, const char *);
int fh_binding_retire(struct binding *);
#pragma GCC visibility pop
#endif
