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
/* Validate the callbacks — the signature window must fit inside each object —
   and reserve initial_capacity slots; NULL reports invalid arguments or
   memory. */
struct gw_binding_store *gw_binding_store_create(
    const struct gw_binding_callbacks *);
/* Addressable object count, occupied plus vacant. */
size_t gw_binding_store_capacity(struct gw_binding_store *);
/* Find or create the binding for (module, guest function, signature of
   exactly length NUL-terminated bytes); *created separates a fresh prepare
   from an existing hit, and the return carries OK, MEMORY, ARGUMENT or the
   prepare's own status. */
int gw_binding_store_get(struct gw_binding_store *, uint64_t, uintptr_t,
                         const char *, size_t, void **, int *);
/* Remove one freshly created entry whose code address was never exposed;
   BUSY when retire refuses because the closure may still execute. */
int gw_binding_store_discard(struct gw_binding_store *, uint64_t, uintptr_t,
                             const char *, size_t);
/* Retire every binding of one module (unload path); continues past refusals
   and reports BUSY if any closure was still active. */
int gw_binding_store_release(struct gw_binding_store *, uint64_t);
/* Free the store; BUSY while any module still owns entries. */
int gw_binding_store_destroy(struct gw_binding_store *);

#pragma GCC visibility pop
#endif
