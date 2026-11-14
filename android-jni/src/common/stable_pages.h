#ifndef GW_STABLE_PAGES_H
#define GW_STABLE_PAGES_H
#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>

/* Shared POD radix directory. Neither libc ever receives the other's malloc
   objects or allocator. Each domain maps zeroed OS pages; CAS publishes them
   once with release/acquire. Losers unmap only their unpublished candidates.
   Published pages are retained for the single runtime's process lifetime.
   Four 8-bit radix levels cover every uint32 block index, with 16 objects per
   leaf. No linked-list walk, movable directory, or fixed-capacity pointer
   table. */
#define FB_PAGE_ITEMS 16

/* Per-domain directory root; all-bits-zero is a valid empty tree. */
struct fb_pages {
  _Atomic(uintptr_t) root;
};

/* Interior radix level: 256 CAS-published child slots. */
struct fb_page_node {
  _Atomic(uintptr_t) child[256];
};

/* Return the page published at slot, mapping a zeroed candidate first when
   create is set. The loser of a concurrent publication unmaps only its own
   candidate; NULL means absent or unmappable, never a half-published page. */
static inline void *fb_publish_page(_Atomic(uintptr_t) *slot, size_t bytes,
                                    int create) {
  uintptr_t found = atomic_load_explicit(slot, memory_order_acquire);
  if (found || !create) {
    return (void *)found;
  }

  void *page = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (page == MAP_FAILED) {
    return NULL;
  }

  uintptr_t expected = 0;
  if (!atomic_compare_exchange_strong_explicit(slot, &expected, (uintptr_t)page,
                                               memory_order_acq_rel,
                                               memory_order_acquire)) {
    munmap(page, bytes);
    return (void *)expected;
  }
  return page;
}

/* Resolve index through three interior levels to its 16-object leaf slot,
   mapping missing pages only when create is set. The returned address is
   stable for the process lifetime once published. */
static inline void *fb_pages_at(struct fb_pages *pages, uint32_t index,
                                size_t stride, int create) {
  if (!stride || stride > SIZE_MAX / FB_PAGE_ITEMS) {
    return NULL;
  }

  struct fb_page_node *node =
      fb_publish_page(&pages->root, sizeof *node, create);
  if (!node) {
    return NULL;
  }
  uint32_t block = index / FB_PAGE_ITEMS;
  for (unsigned shift = 24; shift; shift -= 8) {
    node = fb_publish_page(&node->child[(block >> shift) & 255], sizeof *node,
                           create);
    if (!node) {
      return NULL;
    }
  }

  char *leaf = fb_publish_page(&node->child[block & 255],
                               stride * FB_PAGE_ITEMS, create);
  return leaf ? leaf + (index % FB_PAGE_ITEMS) * stride : NULL;
}
#endif
