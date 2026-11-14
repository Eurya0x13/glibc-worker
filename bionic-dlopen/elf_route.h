#ifndef ELF_ROUTE_H
#define ELF_ROUTE_H
#include "loader_types.h"

/* Classification only: no mapping, relocation, or dependency loading.
 * ERROR never permits fallback to the platform loader. UNMARKED means the
 * supported metadata was parsed successfully without a GNU dependency. */
enum gw_elf_kind { GW_ELF_ERROR = -1, GW_ELF_UNMARKED = 0, GW_ELF_GLIBC = 1 };
enum gw_elf_kind gw_elf_classify(const char *, struct gw_loader_error *);
#endif
