#ifndef GW_DLOPEN_BACKEND_H
#define GW_DLOPEN_BACKEND_H
#include <stddef.h>
#include <stdint.h>
#include "loader_types.h"
#include "module_id.h"

/* Version of this contract; gw_dlopen_query serves only this exact value. */
#define GW_DLOPEN_ABI_V1 1u

/* Output of an open. handle is a logical, generation-bearing id -- never
   convertible to a system dlopen pointer -- and metadata is an opaque
   transport result (the adapter may validate a JNI version in it). The
   backend zeroes it on entry, so failure leaves it clean. */
struct gw_dlopen_library {
  uint64_t handle;
  int32_t metadata; /* Adapter-owned result, e.g. a validated JNI version. */
};

/* All functions execute on the Host. The adapter owns the Guest RPC and any
 * function-call trampoline. context is opaque and may be NULL for a C client.
 * A successful load must finish its own commit; failures must roll back. */
struct gw_dlopen_transport {
  int (*load)(void *context, uint64_t, const char *image, int32_t *metadata,
              struct gw_loader_error *);
  int (*close)(void *context, uint64_t, struct gw_loader_error *);
  int (*find)(void *context, uint64_t, const char *symbol, const char *binding,
              void **entry, struct gw_loader_error *);
  uint64_t (*current_module)(void *context);
};

/* Lifecycle notifications for the observer. OPENED and CLOSED report a
   committed handle; FAILED may report 0 when no slot was ever granted. */
enum gw_dlopen_event { GW_DLOPEN_OPENED, GW_DLOPEN_CLOSED, GW_DLOPEN_FAILED };

/* The wiring installed once per process by gw_dlopen_configure. Every
   function pointer and both context words must stay valid until
   gw_dlopen_stop returns GW_OK. */
struct gw_dlopen_config {
  struct gw_dlopen_transport transport;
  /* Deploys or locates the image for a requested name and writes the
     Guest-visible file path; empty or unterminated output is an image
     error. Owns the deployment policy; the backend owns none. */
  int (*resolve_image)(void *context, const char *requested,
                       char image[GW_LOADER_PATH], struct gw_loader_error *,
                       void *image_context);
  /* Opaque context handed back as resolve_image's last argument. */
  void *image_context;
  /* Called under the module lock. Must not reenter the loader. */
  void (*observe)(int event, uint64_t handle, int32_t metadata, void *);
  void *observer_context;
};

/* Versioned function table. struct_size lets a caller compiled against a
   larger future struct detect that this backend is older and refuse the
   table instead of reading past its end. */
struct gw_dlopen_api_v1 {
  uint32_t abi_version, struct_size;
  int (*open)(void *, const char *, uint32_t, struct gw_dlopen_library *,
              struct gw_loader_error *);
  int (*find)(void *, uint64_t, const char *, const char *, void **,
              struct gw_loader_error *);
  int (*close)(void *, uint64_t, struct gw_loader_error *);
  int (*owns_handle)(uint64_t);
  int (*module_state)(uint64_t);
};

#ifdef __cplusplus
extern "C" {
#endif
/* Returns the static v1 table only for the exact abi_version and a caller
   struct no larger than ours; NULL otherwise. Valid for the process
   lifetime. */
const struct gw_dlopen_api_v1 *gw_dlopen_query(uint32_t, size_t minimum_size);
/* One-time wiring, UNCONFIGURED -> CONFIGURED; a second attempt fails
   GW_E_BUSY (singleton backend, runtime recreation unsupported). */
int gw_dlopen_configure(const struct gw_dlopen_config *,
                        struct gw_loader_error *);
/* CONFIGURED -> READY once the Guest can service requests; opens fail
   GW_E_NOT_READY until then. */
int gw_dlopen_activate(struct gw_loader_error *);
/* Terminal STOPPED transition. Refuses GW_E_BUSY while any module is
   loading/live/closing or an open is in flight; after GW_OK the config
   pointers are released and no further opens are accepted. */
int gw_dlopen_stop(struct gw_loader_error *);
/* Irreversible fail-closed state for unsafe teardown/rollback. Retains all
   module/config pointers until process disposal; refuses further Guest
   opens, borrows, symbol lookups, closes and stop. Does not terminate the
   process or attempt to repair an unknown VM/Guest state. */
void gw_dlopen_quarantine(void);
int gw_dlopen_is_quarantined(void);
/* Pins a module, permitting a matching leave on another thread.
   Succeeds only while READY on a LOADING/LIVE module; on 0 nothing was
   borrowed and the matching leave must be skipped. */
int gw_dlopen_enter(uint64_t);
/* Returns one borrow; the final leave of a CLOSING module lets the closer
   proceed. Must pair 1:1 with a successful enter. */
void gw_dlopen_leave(uint64_t);
/* Executing scope: pins the module and records it on the calling Host thread.
   Includes recursive/ancestor scopes. Close on any locally held module, or
   close requiring a borrow wait while any local scope is held, returns BUSY
   before changing its state. Unwind scopes before retrying; do not spin on
   BUSY. Successful enters must be balanced on the same thread before thread
   exit; scopes cannot be transferred or cancelled. Enter returns 0 on refusal;
   leave returns 0 for a stale, unheld or foreign scope without consuming
   another thread's scope or a transferable pin. */
int gw_dlopen_scope_enter(uint64_t);
int gw_dlopen_scope_leave(uint64_t);
/* Id of the most recently committed open, 0 if none. A snapshot for
   diagnostics only: it pins nothing and proves nothing about state. */
uint64_t gw_dlopen_last_handle(void);
/* Fills the caller-owned diagnostic (message truncated to fit) and returns
   GW_ERROR; error may be NULL when diagnostics are unwanted. */
int gw_dlopen_fail(struct gw_loader_error *, int code, const char *message);
#ifdef __cplusplus
}
#endif
#endif
