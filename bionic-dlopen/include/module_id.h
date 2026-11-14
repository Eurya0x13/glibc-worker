#ifndef GW_MODULE_ID_H
#define GW_MODULE_ID_H
#include <stdint.h>

/* Slot count of the module table: a hard capacity, not a growing
   namespace. */
#define FB_MODULES 64
#if FB_MODULES < 1 || FB_MODULES > 255
#error module index must fit in the handle low byte
#endif
/* "GL" in the top 16 bits marks a Guest logical handle, distinguishing it
   from any raw pointer or bionic dlopen handle the caller may hold. */
#define FB_MODULE_TAG UINT64_C(0x474c000000000000)
/* Isolates the tag field; slot and generation bits are ignored on purpose. */
#define FB_MODULE_MASK UINT64_C(0xffff000000000000)

/* Decodes the slot (0..FB_MODULES-1) from a Guest handle, accepting any
   generation -- including ids retired by slot reuse -- so stale Guest ids
   stay claimed by this backend instead of leaking to the platform loader.
   Returns -1 for anything without the tag. */
static inline int fb_module_index(uint64_t id) {
  unsigned n = id & 255;
  return (id & FB_MODULE_MASK) == FB_MODULE_TAG && n && n <= FB_MODULES
             ? (int)n - 1
             : -1;
}

#endif
