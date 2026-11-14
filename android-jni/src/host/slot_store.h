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
/* Validate the config, reserve initial_capacity slots and owner rows; NULL
   reports invalid arguments or memory. */
struct gw_slot_store *gw_slot_create(const struct gw_slot_config *);
/* Publish one zeroed object under owner (-1 leaves it unowned) and write its
   token; on failure *token stays 0 and nothing is published — owner growth
   and slot reservation happen before any state becomes visible. */
void *gw_slot_insert(struct gw_slot_store *, int owner, uint64_t *token);
/* Resolve a token to its object; NULL when stale, burned, or erased. */
void *gw_slot_get(struct gw_slot_store *, uint64_t token);
/* Dense index scan: visit slot 'index' and receive its current token; NULL
   (and *token 0) when the index is vacant. */
void *gw_slot_at(struct gw_slot_store *, size_t index, uint64_t *token);
/* Walk one owner's dense member list; swap-removal reorders positions, so
   re-read a position after erasing the item it yielded. */
void *gw_slot_owned(struct gw_slot_store *, int owner, size_t position,
                    uint64_t *token);
/* Addressable slot count, live plus vacant; the scan bound for gw_slot_at. */
size_t gw_slot_capacity(struct gw_slot_store *);
/* Retire a live token and its owner membership; 0 for a stale token. Never
   allocates, so it cannot fail for lack of memory. */
int gw_slot_erase(struct gw_slot_store *, uint64_t token);
/* Optional secondary index: attach a live token to a numeric (kind, value)
   key. Failure publishes no index entry; a token can be indexed only once.
   Candidate order is unspecified; C verifies VM class identity. Erasing a
   slot removes its index entry without allocating. */
int gw_slot_index_insert(struct gw_slot_store *, uint64_t token, int kind,
                         uintptr_t value);
uint64_t gw_slot_index_candidate(struct gw_slot_store *, int kind,
                                 uintptr_t value, size_t position);
/* Dense indexed-token traversal, independent of unindexed slot capacity.
   Removal swaps the last member into the erased position; revisit it. */
uint64_t gw_slot_index_at(struct gw_slot_store *, size_t position);
/* Free the whole store; refused (0) while any object is still live. */
int gw_slot_destroy(struct gw_slot_store *);

/* Production adapters supply the Host allocator and generation width. */
struct gw_slot_store *fh_slot_create(size_t size, size_t alignment,
                                     size_t owners, size_t initial_capacity);
#pragma GCC visibility pop
#endif
