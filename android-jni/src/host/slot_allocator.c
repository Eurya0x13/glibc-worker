#include <stdlib.h>
#include "slot_store.h"

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
