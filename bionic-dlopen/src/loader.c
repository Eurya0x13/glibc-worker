/* Host-side loader state machine: logical module handles, borrow accounting
   and load/close transactions. Real GNU work happens in the caller's
   transport on the Guest side; this layer owns no VM and pumps no JNI. */
#define _GNU_SOURCE
#include "dlopen_backend.h"
#include "elf_route.h"
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/stat.h>

/* Backend lifecycle is strictly one-way: configure, activate, stop. No
   transition returns to an earlier state, so a process gets exactly one
   loader instance and cannot reconfigure it. */
enum backend_state { UNCONFIGURED, CONFIGURED, READY, STOPPED };

/* One fixed-slot record. state==0 marks the slot free; generation is bumped
   on reuse, so handles issued by a previous tenant stay invalid. device and
   inode pin the image file against a second concurrent load; requested
   keeps the caller's original name for diagnostics. */
struct module {
  uint64_t id;
  unsigned generation;
  int state, active;
  dev_t device;
  ino_t inode;
  char requested[GW_LOADER_PATH];
};
/* Capacity is a hard 64 slots; exhaustion fails with GW_E_CAPACITY rather
   than queueing or evicting. */
static struct module modules[FB_MODULES];
static struct gw_dlopen_config config;
static enum backend_state state;
/* Opens past their READY check but not yet returned; stop() refuses to run
   while this is nonzero. */
static unsigned inflight_opens;
static _Atomic int quarantined;
/* Handle of the most recent committed open; 0 until the first success. */
static uint64_t last_handle;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
/* Entirely bionic-owned; this condition variable never crosses the bridge. */
static pthread_cond_t quiescent = PTHREAD_COND_INITIALIZER;

void gw_dlopen_quarantine(void) {
  pthread_mutex_lock(&lock);
  atomic_store_explicit(&quarantined, 1, memory_order_release);
  pthread_cond_broadcast(&quiescent);
  pthread_mutex_unlock(&lock);
}

int gw_dlopen_is_quarantined(void) {
  return atomic_load_explicit(&quarantined, memory_order_acquire);
}

static int quarantine_error(struct gw_loader_error *error) {
  return gw_dlopen_fail(error, GW_E_QUARANTINED,
                        "Guest bridge quarantined; discard process");
}

/* Forwards one lifecycle event to the observer, if installed. Keep it cheap:
   it may run while the module lock is held, and FAILED may carry id 0 when
   no slot was ever granted. */
static void observe(int event, uint64_t id, int32_t metadata) {
  if (config.observe) {
    config.observe(event, id, metadata, config.observer_context);
  }
}

/* Public error helper: stores the code plus a truncated message in the
   caller's diagnostic (NULL allowed) and always returns GW_ERROR. */
int gw_dlopen_fail(struct gw_loader_error *error, int code,
                   const char *message) {
  if (error) {
    error->code = code;
    snprintf(error->message, sizeof error->message, "%s", message);
  }

  return GW_ERROR;
}

/* Entry points zero the diagnostic up front so a success never inherits
   stale error text from an earlier failure. */
static void clear_error(struct gw_loader_error *error) {
  if (error) {
    memset(error, 0, sizeof *error);
  }
}

/* True for any tag-shaped id, including generations retired after slot
   reuse. Retired handles must still be claimed here so a Guest id can
   never leak to the platform loader as an unknown value. */
static int owns_handle(uint64_t id) {
  return fb_module_index(id) >= 0;
}

/* Slot lookup honoring generation: NULL when the slot is free or now owned
   by a newer handle. Callers must hold the module lock. */
static struct module *get_module(uint64_t id) {
  int n = fb_module_index(id);
  return n >= 0 && modules[n].state && modules[n].id == id ? &modules[n] : NULL;
}

/* Locked snapshot exported through the vtable. Unknown and retired ids
   report GW_MODULE_STALE; LOADING, LIVE and CLOSING expose the module
   state machine as stored. */
static int module_state(uint64_t id) {
  pthread_mutex_lock(&lock);
  struct module *m = get_module(id);
  int result = m ? m->state : GW_MODULE_STALE;
  pthread_mutex_unlock(&lock);
  return result;
}

/* Thread-safe read of the last committed handle. A snapshot only: it grants
   no borrow and pins no module. */
uint64_t gw_dlopen_last_handle(void) {
  pthread_mutex_lock(&lock);
  uint64_t id = last_handle;
  pthread_mutex_unlock(&lock);
  return id;
}

/* Takes one borrow on a LOADING or LIVE module while the backend is READY.
   Returns 0 without side effects when refused, so a failed enter must not
   be paired with leave. */
int gw_dlopen_enter(uint64_t id) {
  pthread_mutex_lock(&lock);
  struct module *m = get_module(id);
  int ok = !quarantined && state == READY && m &&
           (m->state == GW_MODULE_LOADING || m->state == GW_MODULE_LIVE);
  if (ok) {
    m->active++;
  }
  pthread_mutex_unlock(&lock);
  return ok;
}

/* Releases one borrow. The final leave of a CLOSING module wakes the closer
   thread; leaves on stale or unborrowed ids are ignored, not counted. */
void gw_dlopen_leave(uint64_t id) {
  pthread_mutex_lock(&lock);
  struct module *m = get_module(id);
  if (m && m->active > 0) {
    m->active--;
    if (!m->active && m->state == GW_MODULE_CLOSING) {
      pthread_cond_broadcast(&quiescent);
    }
  }
  pthread_mutex_unlock(&lock);
}

/* Installs transport, image resolver and observer exactly once
   (UNCONFIGURED -> CONFIGURED); later attempts fail GW_E_BUSY. Every
   pointer and context word in *value must stay valid until stop succeeds. */
int gw_dlopen_configure(const struct gw_dlopen_config *value,
                        struct gw_loader_error *error) {
  clear_error(error);
  if (!value || !value->resolve_image || !value->transport.load ||
      !value->transport.find || !value->transport.close ||
      !value->transport.current_module) {
    return gw_dlopen_fail(error, GW_E_ARGUMENT,
                          "Incomplete Guest loader configuration");
  }

  pthread_mutex_lock(&lock);
  if (quarantined) {
    pthread_mutex_unlock(&lock);
    return quarantine_error(error);
  }

  if (state != UNCONFIGURED) {
    pthread_mutex_unlock(&lock);
    return gw_dlopen_fail(
        error, GW_E_BUSY,
        "Guest loader already configured; runtime recreation unsupported");
  }

  config = *value;
  state = CONFIGURED;
  pthread_mutex_unlock(&lock);
  printf("LOADER_BACKEND configured=1 abi=%u\n", GW_DLOPEN_ABI_V1);
  return GW_OK;
}

/* CONFIGURED -> READY. Opens are refused until this point so the Guest
   runtime can finish booting before the first image is requested. */
int gw_dlopen_activate(struct gw_loader_error *error) {
  clear_error(error);
  pthread_mutex_lock(&lock);
  if (quarantined) {
    pthread_mutex_unlock(&lock);
    return quarantine_error(error);
  }

  if (state != CONFIGURED) {
    pthread_mutex_unlock(&lock);
    return gw_dlopen_fail(error, GW_E_NOT_READY,
                          "Guest loader cannot activate in current state");
  }

  state = READY;
  pthread_mutex_unlock(&lock);
  printf("LOADER_BACKEND ready=1\n");
  return GW_OK;
}

/* Terminal transition to STOPPED, refused with GW_E_BUSY while any module
   is loading/live/closing or an open is in flight. The caller must drain
   borrows and close modules first; after GW_OK the config pointers are
   released. */
int gw_dlopen_stop(struct gw_loader_error *error) {
  clear_error(error);
  pthread_mutex_lock(&lock);
  if (quarantined) {
    pthread_mutex_unlock(&lock);
    return quarantine_error(error);
  }

  if (state != READY && state != CONFIGURED) {
    pthread_mutex_unlock(&lock);
    return gw_dlopen_fail(error, GW_E_NOT_READY, "Guest loader is not running");
  }

  int busy = inflight_opens != 0;
  for (int i = 0; i < FB_MODULES; i++) {
    if (modules[i].state) {
      busy = 1;
    }
  }
  if (busy) {
    pthread_mutex_unlock(&lock);
    return gw_dlopen_fail(
        error, GW_E_BUSY,
        "Guest loader still owns live/loading/closing images");
  }

  state = STOPPED;
  pthread_mutex_unlock(&lock);
  printf("LOADER_BACKEND stopped=1\n");
  return GW_OK;
}

/* Grants a slot and returns a fresh-generation handle in LOADING state.
   The image must already be an existing regular file; the same
   device/inode may not be active twice (GW_E_DUPLICATE); exhausted slots
   or generations give GW_E_CAPACITY. Refusal fills the error and returns
   0 without touching module state. */
static uint64_t reserve(const char *path, const char *image,
                        struct gw_loader_error *error) {
  struct stat st;
  if (stat(image, &st) || !S_ISREG(st.st_mode)) {
    gw_dlopen_fail(error, GW_E_IMAGE, "Guest image is not a regular file");
    return 0;
  }

  pthread_mutex_lock(&lock);
  if (quarantined) {
    pthread_mutex_unlock(&lock);
    quarantine_error(error);
    return 0;
  }

  for (int i = 0; i < FB_MODULES; i++) {
    if (modules[i].state && modules[i].device == st.st_dev &&
        modules[i].inode == st.st_ino) {
      if (error) {
        error->code = GW_E_DUPLICATE;
        snprintf(error->message, sizeof error->message,
                 "Installed Guest image already owned by another load: %.180s",
                 modules[i].requested);
      }
      pthread_mutex_unlock(&lock);
      return 0;
    }
  }

  uint64_t id = 0;
  for (int i = 0; i < FB_MODULES; i++) {
    if (!modules[i].state && modules[i].generation < UINT_MAX) {
      struct module *m = &modules[i];
      m->generation++;
      id = FB_MODULE_TAG | ((uint64_t)m->generation << 8) | (unsigned)(i + 1);
      m->id = id;
      m->state = GW_MODULE_LOADING;
      m->active = 0;
      m->device = st.st_dev;
      m->inode = st.st_ino;
      snprintf(m->requested, sizeof m->requested, "%s", path);
      break;
    }
  }

  pthread_mutex_unlock(&lock);
  if (!id) {
    gw_dlopen_fail(error, GW_E_CAPACITY, "Guest library table exhausted");
  }
  return id;
}

/* Full load transaction behind the vtable's open. AUTO (flags==0) first
   classifies the ELF: GW_PASS hands the path to the caller's platform
   loader, GW_ERROR is terminal and must not be retried there. Otherwise
   reserve, transport.load, then commit LIVE and publish the handle or roll
   the slot back. Quarantine retains uncertain resources and the LOADING slot
   without publishing a usable handle. */
static int loader_open(void *context, const char *path, uint32_t flags,
                       struct gw_dlopen_library *result,
                       struct gw_loader_error *error) {
  clear_error(error);
  if (result) {
    memset(result, 0, sizeof *result);
  }
  if (!path || !path[0] || strlen(path) >= GW_LOADER_PATH || !result ||
      (flags & ~GW_OPEN_GUEST)) {
    return gw_dlopen_fail(error, GW_E_ARGUMENT, "Invalid Guest open arguments");
  }
  if (!(flags & GW_OPEN_GUEST)) {
    enum gw_elf_kind kind = gw_elf_classify(path, error);
    if (kind == GW_ELF_ERROR) {
      return GW_ERROR;
    }
    if (kind == GW_ELF_UNMARKED) {
      return GW_PASS;
    }
  }

  pthread_mutex_lock(&lock);
  if (quarantined) {
    pthread_mutex_unlock(&lock);
    return quarantine_error(error);
  }

  if (state != READY) {
    pthread_mutex_unlock(&lock);
    return gw_dlopen_fail(error, GW_E_NOT_READY, "Guest loader is not ready");
  }
  /* Registered only after READY was confirmed, so stop() cannot pass while
     this open is between classification and commit. */
  inflight_opens++;
  pthread_mutex_unlock(&lock);

  char image[GW_LOADER_PATH] = {0};
  uint64_t id = 0;
  int rc = GW_ERROR;

  if (config.resolve_image(context, path, image, error, config.image_context) !=
      GW_OK) {
    goto done;
  }
  if (!image[0] || !memchr(image, 0, sizeof image)) {
    gw_dlopen_fail(error, GW_E_IMAGE,
                   "Image resolver returned an invalid path");
    goto done;
  }
  id = reserve(path, image, error);
  if (!id) {
    observe(GW_DLOPEN_FAILED, id, 0);
    goto done;
  }

  int32_t metadata = 0;
  int accepted =
      config.transport.load(context, id, image, &metadata, error) == GW_OK;

  pthread_mutex_lock(&lock);
  struct module *m = get_module(id);
  if (quarantined) {
    /* A failed rollback may leave VM entries pointing into this image.
       Keep the slot, generation and transport resources permanently. */
    accepted = 0;
    if (error) {
      error->code = GW_E_QUARANTINED;
    }
    if (!error || !error->message[0]) {
      quarantine_error(error);
    }
    observe(GW_DLOPEN_FAILED, id, 0);
  } else if (accepted) {
    m->state = GW_MODULE_LIVE;
    last_handle = id;
    observe(GW_DLOPEN_OPENED, id, metadata);
    result->handle = id;
    result->metadata = metadata;
    rc = GW_OK;
  } else {
    m->state = 0;
    m->id = 0;
    observe(GW_DLOPEN_FAILED, id, 0);
  }
  pthread_mutex_unlock(&lock);

  if (accepted) {
    printf("LOADER_BACKEND operation=open handle=%llx image=%s\n",
           (unsigned long long)id, image);
  }

done:
  pthread_mutex_lock(&lock);
  inflight_opens--;
  pthread_mutex_unlock(&lock);
  return rc;
}

/* Symbol resolution under a borrow, so the image cannot be unloaded behind
   an entry handed to the caller. Non-tag ids PASS (not ours to answer);
   ours-but-stale fails GW_E_HANDLE instead of passing through. */
static int loader_find(void *context, uint64_t id, const char *symbol,
                       const char *descriptor, void **entry,
                       struct gw_loader_error *error) {
  clear_error(error);
  if (entry) {
    *entry = NULL;
  }
  if (!owns_handle(id)) {
    return GW_PASS;
  }

  if (gw_dlopen_is_quarantined()) {
    return quarantine_error(error);
  }
  if (!symbol || !symbol[0] || strlen(symbol) >= GW_LOADER_SIGNATURE ||
      !entry) {
    return gw_dlopen_fail(error, GW_E_ARGUMENT,
                          "Valid symbol and output are required");
  }
  if (!gw_dlopen_enter(id)) {
    return gw_dlopen_fail(error, GW_E_HANDLE,
                          "Guest library handle is stale or closing");
  }

  int rc = config.transport.find(context, id, symbol, descriptor, entry, error);
  gw_dlopen_leave(id);
  return rc;
}

/* Close synchronization: LIVE -> CLOSING refuses new borrows, waits out
   active ones, then delegates teardown to the transport. A recoverable close
   failure restores LIVE; quarantine retains CLOSING and success frees the
   slot for a later generation. */
static int loader_close(void *context, uint64_t id,
                        struct gw_loader_error *error) {
  clear_error(error);
  if (!owns_handle(id)) {
    return GW_PASS;
  }

  pthread_mutex_lock(&lock);
  if (quarantined) {
    pthread_mutex_unlock(&lock);
    return quarantine_error(error);
  }

  struct module *m = get_module(id);
  if (state != READY || !m || m->state != GW_MODULE_LIVE) {
    pthread_mutex_unlock(&lock);
    return gw_dlopen_fail(error, GW_E_HANDLE,
                          "Guest library handle is stale or closing");
  }
  pthread_mutex_unlock(&lock);

  /* Self-unload guard: closing from the image's own active callback would
     wait for the caller to leave, which it cannot do while blocked here. */
  if (config.transport.current_module(context) == id) {
    return gw_dlopen_fail(
        error, GW_E_BUSY,
        "Cannot unload a Guest image from its own active callback");
  }

  /* Re-validate under the lock: the module may have been retired while the
     current_module probe ran without it. */
  pthread_mutex_lock(&lock);
  if (quarantined) {
    pthread_mutex_unlock(&lock);
    return quarantine_error(error);
  }
  m = get_module(id);
  if (!m || m->state != GW_MODULE_LIVE) {
    pthread_mutex_unlock(&lock);
    return gw_dlopen_fail(error, GW_E_HANDLE,
                          "Guest library handle is stale or closing");
  }
  m->state = GW_MODULE_CLOSING;
  /* Active borrowers finish before transport teardown. The adapter services
     any callbacks; this layer never owns a VM or pumps JNI. */
  while (m->active && !quarantined) {
    pthread_cond_wait(&quiescent, &lock);
  }
  if (quarantined) {
    pthread_mutex_unlock(&lock);
    return quarantine_error(error);
  }
  pthread_mutex_unlock(&lock);

  int rc = config.transport.close(context, id, error);
  pthread_mutex_lock(&lock);
  if (quarantined) {
    rc = quarantine_error(error);
  } else if (rc == GW_OK) {
    m->state = 0;
    m->id = 0;
    observe(GW_DLOPEN_CLOSED, id, 0);
  } else {
    m->state = GW_MODULE_LIVE;
  }
  pthread_mutex_unlock(&lock);

  return rc;
}
/* The v1 function table handed out by gw_dlopen_query. Handles crossing it
   are logical ids with generations, never raw dlopen pointers. */
static const struct gw_dlopen_api_v1 api = {
    GW_DLOPEN_ABI_V1, sizeof api,  loader_open, loader_find,
    loader_close,     owns_handle, module_state};

/* ABI gate: the exact known version and a caller struct no larger than ours
   earn the static vtable; anything else gets NULL. Valid for the process
   lifetime. */
const struct gw_dlopen_api_v1 *gw_dlopen_query(uint32_t abi_version,
                                               size_t minimum_size) {
  return abi_version == GW_DLOPEN_ABI_V1 && minimum_size <= sizeof api ? &api
                                                                       : NULL;
}
