/* Load/close transaction phases. VM entry retirement must succeed while the
   Guest image is still mapped; uncertainty is terminal for this bridge. */
#include <stdio.h>
#include "host_internal.h"
#include "dlopen_backend.h"

struct load_frame {
  uint64_t module;
  int local_depth;
};

/* Open a loader transaction on the calling context: quarantine gate, then a
   fresh 1024-local frame with the module and depth saved. Fails before
   mutating the context, so the caller never unwinds a half-installed
   transaction. */
static int enter_frame(struct host_context *c, uint64_t id,
                       struct load_frame *saved,
                       struct gw_loader_error *error) {
  if (gw_dlopen_is_quarantined()) {
    return gw_loader_fail(error, GW_E_QUARANTINED,
                          "Guest bridge quarantined; discard process");
  }

  *saved = (struct load_frame){c->module, c->local_depth};
  if ((*c->env)->PushLocalFrame(c->env, 1024) != JNI_OK) {
    return gw_loader_fail(error, GW_E_BRIDGE,
                          "Cannot create loader local frame");
  }

  c->module = id;
  c->local_depth++;
  return GW_OK;
}

/* Close the transaction: pop the frame, retire every local minted inside it,
   restore the saved module and depth. */
static void leave_frame(struct host_context *c, struct load_frame saved) {
  (*c->env)->PopLocalFrame(c->env, NULL);
  fh_drop_locals(c, saved.local_depth + 1);
  c->local_depth = saved.local_depth;
  c->module = saved.module;
}

/* Detach the pending exception (or return NULL) so cleanup JNI may run. */
static jthrowable take_exception(JNIEnv *env) {
  jthrowable pending = (*env)->ExceptionOccurred(env);
  if (pending) {
    (*env)->ExceptionClear(env);
  }
  return pending;
}

/* Re-raise the rollback's cause after cleanup, discarding any exception the
   cleanup itself raised. */
static void restore_exception(JNIEnv *env, jthrowable original) {
  if (original) {
    /* Cleanup may itself fail. Preserve the exception that caused rollback. */
    if ((*env)->ExceptionCheck(env)) {
      (*env)->ExceptionClear(env);
    }
    (*env)->Throw(env, original);
    (*env)->DeleteLocalRef(env, original);
  }
}

/* Keep the earliest failure; later exceptions from rollback steps are
   dropped. */
static void merge_exception(JNIEnv *env, jthrowable *original) {
  jthrowable next = take_exception(env);
  if (!*original) {
    *original = next;
  } else if (next) {
    (*env)->DeleteLocalRef(env, next);
  }
}

/* Terminal outcome: quarantine the bridge; retained resources are never
   freed after this. */
static int quarantine(struct gw_loader_error *error, const char *reason) {
  gw_dlopen_quarantine();
  return gw_loader_fail(error, GW_E_QUARANTINED, reason);
}

/* Guest load transaction: run JNI_OnLoad and commit on an accepted version;
   on rejection unregister, abort and sweep so no partial binding survives.
   Any uncertain rollback quarantines the process rather than freeing
   resources that may still be reachable. */
jint fh_transport_load(JNIEnv *env, uint64_t id, const char *image,
                       struct gw_loader_error *error) {
  struct host_context *c = fh_context(env, error);
  struct load_frame saved;
  if (!c || enter_frame(c, id, &saved, error) != GW_OK) {
    return JNI_ERR;
  }

  jvalue args[FB_ARGS + 2] = {{.l = (jobject)image}};
  jvalue out = fh_command(c, FB_LOAD, 0, NULL, args);
  int accepted = !c->command_error &&
                 gw_jni_accepts(gw_host_jni_version, out.i) &&
                 !(*env)->ExceptionCheck(env);
  if (accepted) {
    fh_command(c, FB_COMMIT_LOAD, 0, NULL, NULL);
    if (c->command_error) {
      quarantine(error,
                 "Guest commit failed; resources retained; discard process");
      accepted = 0;
    }
  } else {
    gw_loader_fail(
        error,
        c->command_error   ? GW_E_BRIDGE
        : c->diagnostic[0] ? GW_E_LOAD
                           : GW_E_VERSION,
        c->diagnostic[0]
            ? c->diagnostic
            : "Guest JNI_OnLoad version rejected or exception pending");
    jthrowable pending = take_exception(env);
    if (release_registrations(env, id) != JNI_OK) {
      quarantine(error,
                 "Load rollback cannot unregister VM entries; resources "
                 "retained; discard process");
    } else {
      fh_command(c, FB_ABORT_LOAD, 0, NULL, NULL);
      merge_exception(env, &pending);
      if (c->command_error) {
        quarantine(
            error,
            "Guest rollback failed; resources retained; discard process");
      } else {
        release_bindings(id);
        sweep_ids(env, 0);
      }
    }
    restore_exception(env, pending);
  }
  leave_frame(c, saved);
  return accepted ? out.i : JNI_ERR;
}

/* Guest unload transaction: prepare (JNI_OnUnload), remove VM native
   entries, then dlclose; only a fully successful pass releases the module's
   bindings and ID-cache entries. */
int fh_transport_close(JNIEnv *env, uint64_t id,
                       struct gw_loader_error *error) {
  struct host_context *c = fh_context(env, error);
  struct load_frame saved;
  if (!c) {
    return GW_ERROR;
  }
  if (enter_frame(c, id, &saved, error) != GW_OK) {
    return GW_ERROR;
  }

  /* OnUnload runs once, then Host removes VM native entries, then dlclose.
     Failure in any phase retains every resource still potentially reachable. */
  fh_command(c, FB_PREPARE_UNLOAD, 0, NULL, NULL);
  jthrowable pending = take_exception(env);
  int rc;
  if (c->command_error) {
    rc = quarantine(error, "Guest unload preparation failed; discard process");
  } else if (release_registrations(env, id) != JNI_OK) {
    rc = quarantine(error,
                    "Unload cannot unregister VM entries; resources retained; "
                    "discard process");
  } else {
    fh_command(c, FB_UNLOAD, 0, NULL, NULL);
    merge_exception(env, &pending);
    if (c->command_error) {
      rc = quarantine(
          error, "Guest dlclose failed; closures retained; discard process");
    } else {
      release_bindings(id);
      sweep_ids(env, 0);
      rc = GW_OK;
    }
  }
  restore_exception(env, pending);
  leave_frame(c, saved);
  return rc;
}

/* Resolve one symbol into a trampoline binding owned by the image; *entry
   stays executable and valid until the module's bindings are released. */
int fh_transport_find(JNIEnv *env, uint64_t id, const char *symbol,
                      const char *descriptor, void **entry,
                      struct gw_loader_error *error) {
  struct host_context *c = fh_context(env, error);
  if (!c) {
    return GW_ERROR;
  }

  uint64_t previous = c->module;
  c->module = id;
  jvalue found = fh_command(c, FB_LOOKUP, 0, symbol, NULL);
  struct binding *binding =
      !c->command_error && found.j
          ? make_binding(id, (uintptr_t)found.j, descriptor, error)
          : NULL;
  c->module = previous;

  if (c->command_error) {
    return gw_loader_fail(
        error, GW_E_BRIDGE,
        c->diagnostic[0] ? c->diagnostic : "Guest lookup failed");
  }
  if (!found.j) {
    return GW_NOT_FOUND;
  }
  if (!binding) {
    return GW_ERROR;
  }

  *entry = binding->code;
  return GW_OK;
}
