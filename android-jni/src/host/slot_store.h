#ifndef GW_SLOT_STORE_H
#define GW_SLOT_STORE_H
#include <stddef.h>
#include <stdint.h>
#pragma GCC visibility push(hidden)

/* Private, externally serialized storage ABI. C owns object contents and VM
   resources. The store owns stable zeroed slots and dense per-owner indexes.
   Tokens are opaque 64-bit integers: high generation, low index+1. A slot
   reaching generation_max is permanently retired, never wrapped. No live
   object may remain at destroy. Failed insertions publish neither object nor
   owner membership. at/owned pointers remain valid until erase/destroy.
   initial_capacity reserves storage at creation; the store then grows in
   blocks. owner_count is the initial owner-vector reservation; nonnegative
   owner IDs grow the private owner directory as needed, before publication. */
struct gw_slot_config {
  size_t object_size, object_alignment, owner_count, initial_capacity;
  uint32_t generation_max;
  void *context;
  void *(*allocate)(void *, size_t, size_t);
  void (*deallocate)(void *, void *);
};
struct gw_slot_store;
struct gw_slot_store *gw_slot_create(const struct gw_slot_config *);
void *gw_slot_insert(struct gw_slot_store *, int owner, uint64_t *token);
void *gw_slot_get(struct gw_slot_store *, uint64_t token);
void *gw_slot_at(struct gw_slot_store *, size_t index, uint64_t *token);
void *gw_slot_owned(struct gw_slot_store *, int owner, size_t position,
                    uint64_t *token);
size_t gw_slot_capacity(struct gw_slot_store *);
int gw_slot_erase(struct gw_slot_store *, uint64_t token);
int gw_slot_destroy(struct gw_slot_store *);

/* Production adapters supply the Host allocator and generation width. */
struct gw_slot_store *fh_slot_create(size_t size, size_t alignment,
                                     size_t owners, size_t initial_capacity);
#pragma GCC visibility pop
#endif
