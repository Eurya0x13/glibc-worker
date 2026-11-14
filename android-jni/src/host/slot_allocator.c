#include <stdlib.h>
#include "slot_store.h"

/* Store internals come from the Host heap; NULL surfaces as an insertion or
   creation failure. Sub-pointer alignments are raised to the minimum
   posix_memalign accepts. */
static void *allocate(void *context, size_t size, size_t alignment) {
  (void)context;
  void *memory = NULL;
  if (alignment < sizeof(void *)) {
    alignment = sizeof(void *);
  }
  return posix_memalign(&memory, alignment, size) ? NULL : memory;
}

/* Frees only whole allocate results: blocks and the store itself, never a
   partial object. */
static void deallocate(void *context, void *memory) {
  (void)context;
  free(memory);
}

/* Pin a store to this runtime: Host heap, hidden ABI, and the full 32-bit
   generation space, so an index is permanently retired after extreme reuse
   instead of wrapping into old tokens. owners and initial_capacity are
   reservations; NULL means the Host could not supply the initial block. */
struct gw_slot_store *fh_slot_create(size_t size, size_t alignment,
                                     size_t owners, size_t initial_capacity) {
  const struct gw_slot_config config = {
      .object_size = size,
      .object_alignment = alignment,
      .owner_count = owners,
      .initial_capacity = initial_capacity,
      .generation_max = UINT32_MAX,
      .allocate = allocate,
      .deallocate = deallocate,
  };
  return gw_slot_create(&config);
}
