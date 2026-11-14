#ifndef GW_IMAGE_POLICY_H
#define GW_IMAGE_POLICY_H
#include "loader_backend.h"

/* JNI pinning for the GuestImages helper, held from full_host_start until
   full_host_shutdown so the class and method IDs stay valid VM-wide. */
struct gw_image_policy {
  jclass helper;
  jmethodID resolve;
};

/* Bind the helper class with a global ref and run its one-shot configure.
   Every failure path drops the ref before returning. */
int gw_image_policy_start(struct gw_image_policy *, JNIEnv *,
                          const char *installed_dir, const char *catalog,
                          int allow_direct, struct gw_loader_error *);
/* Loader transport hook: map a requested library path to the image the
   Guest may actually open, or reject the request with GW_E_IMAGE. */
int gw_image_policy_resolve(JNIEnv *, const char *, char image[GW_LOADER_PATH],
                            struct gw_loader_error *, void *);
/* Release the global ref; idempotent and callable on any failed start. */
void gw_image_policy_finish(struct gw_image_policy *, JNIEnv *);
#endif
