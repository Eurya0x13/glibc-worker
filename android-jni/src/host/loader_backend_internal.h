#ifndef GW_LOADER_BACKEND_INTERNAL_H
#define GW_LOADER_BACKEND_INTERNAL_H
#include "loader_backend.h"
#include "full_protocol.h"

/* Implemented by the JNI/RPC bridge. load() includes OnLoad validation,
   COMMIT or ABORT and registration/closure rollback. close() prepares OnUnload,
   retires VM entries, then dlcloses. Uncertain cleanup quarantines the bridge
   and retains reachable resources; the host must discard the process. */
struct gw_guest_transport {
  jint (*load)(JNIEnv *, uint64_t, const char *image, struct gw_loader_error *);
  int (*close)(JNIEnv *, uint64_t, struct gw_loader_error *);
  int (*find)(JNIEnv *, uint64_t, const char *, const char *, void **,
              struct gw_loader_error *);
  uint64_t (*current_module)(JNIEnv *);
};

typedef int (*gw_image_resolver)(JNIEnv *, const char *,
                                 char image[GW_LOADER_PATH],
                                 struct gw_loader_error *, void *);

struct gw_loader_config {
  struct gw_guest_transport transport;
  gw_image_resolver resolve_image;
  void *image_context;
  struct full_bridge *counters;
};

/* Configure once before publishing the backend. Activate only once Guest RPC
   is serving. Stop refuses live modules and leaves the backend usable. */
int gw_loader_configure(const struct gw_loader_config *,
                        struct gw_loader_error *);
int gw_loader_activate(struct gw_loader_error *);
int gw_loader_stop(struct gw_loader_error *);
int gw_loader_enter(uint64_t handle);
void gw_loader_leave(uint64_t handle);
int gw_loader_scope_enter(uint64_t handle);
int gw_loader_scope_leave(uint64_t handle);
uint64_t gw_loader_last_handle(void);
int gw_loader_version_supported(jint version);
int gw_loader_fail(struct gw_loader_error *, int code, const char *message);
#endif
