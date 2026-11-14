#ifndef GW_BINDING_STORE_H
#define GW_BINDING_STORE_H
#include <stddef.h>
#include <stdint.h>
#pragma GCC visibility push(hidden)

/* Private C ABI for the Host store. The caller serializes all operations.
   Objects and their embedded NUL-terminated signatures never move. prepare
   initializes an unpublished object, copies its signature to the configured
   offset and cleans up its own failure. retire destroys owned resources only
   when no VM/caller can still enter its closure, leaving signature bytes
   intact until the registry removes the index entry. */
enum gw_binding_result {
  GW_BINDING_OK,
  GW_BINDING_MEMORY,
  GW_BINDING_ARGUMENT,
  GW_BINDING_FFI,
  GW_BINDING_BUSY
};

struct gw_binding_callbacks {
  size_t object_size, object_alignment, signature_offset, signature_capacity;
  size_t initial_capacity;
  void *context;
  void *(*allocate)(void *, size_t, size_t);
  void (*deallocate)(void *, void *);
  int (*prepare)(void *, void *, uint64_t, uintptr_t, const char *);
  int (*retire)(void *, void *);
};

struct gw_binding_store;
struct gw_binding_store *gw_binding_store_create(
    const struct gw_binding_callbacks *);
size_t gw_binding_store_capacity(struct gw_binding_store *);
int gw_binding_store_get(struct gw_binding_store *, uint64_t, uintptr_t,
                         const char *, size_t, void **, int *);
int gw_binding_store_discard(struct gw_binding_store *, uint64_t, uintptr_t,
                             const char *, size_t);
int gw_binding_store_release(struct gw_binding_store *, uint64_t);
int gw_binding_store_destroy(struct gw_binding_store *);

#pragma GCC visibility pop
#endif
