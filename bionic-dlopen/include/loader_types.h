#ifndef GW_LOADER_TYPES_H
#define GW_LOADER_TYPES_H
#include <stdint.h>

/* Path and image buffer capacity including NUL; longer requests are
   argument errors, never truncated. */
#define GW_LOADER_PATH 4096
/* Diagnostic message capacity; longer text is truncated to fit. */
#define GW_LOADER_DIAGNOSTIC 512
/* Symbol and binding-descriptor buffer capacity including NUL. */
#define GW_LOADER_SIGNATURE 256

/* PASS alone permits the caller to use its original platform loader. */
enum gw_loader_status {
  GW_ERROR = -1,
  GW_PASS = 0,
  GW_OK = 1,
  GW_NOT_FOUND = 2
};

/* Stable error vocabulary shared by every layer; GW_E_NONE means "no
   error". Values are contractual -- hosts and tests match on them. */
enum gw_loader_code {
  GW_E_NONE,
  GW_E_ARGUMENT,
  GW_E_NOT_READY,
  GW_E_IMAGE,
  GW_E_DUPLICATE,
  GW_E_CAPACITY,
  GW_E_LOAD,
  GW_E_VERSION,
  GW_E_HANDLE,
  GW_E_BUSY,
  GW_E_BRIDGE
};

/* AUTO lets ELF classification pick the domain; GUEST asserts the GNU
   domain up front and skips classification entirely. */
enum gw_loader_open_flags { GW_OPEN_AUTO = 0, GW_OPEN_GUEST = 1 };

/* Logical per-module state. A zero value (not named here) marks a free
   slot; STALE is the query answer for unknown or retired handles and is
   never stored. */
enum gw_module_state {
  GW_MODULE_STALE = -1,
  GW_MODULE_LOADING = 1,
  GW_MODULE_LIVE = 2,
  GW_MODULE_CLOSING = 3
};

/* Caller-owned diagnostic. Successful calls leave it zeroed; the content
   is valid only until the next call fills the same object. */
struct gw_loader_error {
  int code;
  char message[GW_LOADER_DIAGNOSTIC];
};

#endif
