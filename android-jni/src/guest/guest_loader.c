#define _GNU_SOURCE
#include <jni.h>
#include <ffi.h>
#include <dlfcn.h>
#include <link.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "protocol.h"
#include "full_bridge.h"
#include "jni_signature.h"
#include "guest_internal.h"

/* One slot per guest image, indexed by the low byte of the module id. The
   slot is claimed under modules_lock before dlopen runs so two loads of the
   same id cannot both proceed; the lock is never held across dlopen/dlclose
   or JNI_OnLoad, which may re-enter the bridge as nested JNI calls. */
struct guest_module {
  uint64_t id;
  void *handle;
  struct link_map *map;
  int committed, unload_prepared;
};
static struct guest_module modules[FB_MODULES];
static pthread_mutex_t modules_lock = PTHREAD_MUTEX_INITIALIZER;

/* Guest-side dispatcher for one posted fb_call. Runs on the worker that owns
   ctx[gid], so the plain (non-atomic) fields need no locking: the
   request/response sequence words are the only shared state, paired as an
   acquire here and a release before the completion notify. Every path exits
   through "done" to restore the caller's gmodule attribution. */
void fg_execute(struct fb_call *cmd) {
  int seq = atomic_load_explicit(&cmd->request, memory_order_acquire);
  if (seq == atomic_load_explicit(&cmd->response, memory_order_relaxed)) {
    return;
  }

#if GW_RPC_PROFILE
  unsigned handoff_depth = (unsigned)gdepth;
  struct gw_handoff_row *handoff = gw_rpc_peer_handoff((unsigned)gid);
  if (cmd->op == FB_NATIVE && handoff_depth < GW_HANDOFF_DEPTH) {
    gw_handoff_dispatch(handoff, handoff_depth);
  }
#endif

  cmd->result = (jvalue){0};
  cmd->error = 0;
  cmd->diagnostic[0] = 0;
  uint64_t previous = gmodule;
  gmodule = cmd->module;
  int index = fb_module_index(cmd->module);
  struct guest_module *m = index < 0 ? NULL : &modules[index];

  if (cmd->op == FB_OWNER) {
    /* Reverse lookup: map a guest code address back to its owning module by
       link_map identity. The result stays zero when no live image claims
       the address; that is an answer, not an error. */
    Dl_info info = {0};
    struct link_map *map = NULL;
    if (dladdr1((void *)cmd->function, &info, (void **)&map, RTLD_DL_LINKMAP)) {
      pthread_mutex_lock(&modules_lock);
      for (int i = 0; i < FB_MODULES; i++) {
        if (modules[i].id && modules[i].map == map) {
          cmd->result.j = (jlong)modules[i].id;
          break;
        }
      }
      pthread_mutex_unlock(&modules_lock);
    }
    goto done;
  }

  if (!m) {
    cmd->error = 7;
    goto done;
  }

  if (cmd->op == FB_LOAD) {
    /* Two-phase load: dlopen plus JNI_OnLoad run here, but the image stays
       provisional until the Host answers the returned version with
       FB_COMMIT_LOAD or FB_ABORT_LOAD. RTLD_LOCAL keeps one image's symbols
       from satisfying another image's links. */
    pthread_mutex_lock(&modules_lock);
    if (m->id) {
      pthread_mutex_unlock(&modules_lock);
      cmd->error = 1;
      goto done;
    }
    m->id = cmd->module;
    pthread_mutex_unlock(&modules_lock);

    const char *path = (const char *)cmd->args[0].l;
    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
      snprintf(cmd->diagnostic, sizeof cmd->diagnostic, "%s", dlerror());
      cmd->result.i = JNI_ERR;
      goto done;
    }

    struct link_map *map = NULL;
    dlinfo(handle, RTLD_DI_LINKMAP, &map);
    pthread_mutex_lock(&modules_lock);
    m->handle = handle;
    m->map = map;
    pthread_mutex_unlock(&modules_lock);

    Dl_info libc = {0};
    dladdr((void *)malloc, &libc);
    atomic_fetch_add(&G->image_opens, 1);
    dprintf(2,
            "GUEST_IMAGE_EVENT phase=open handle=%llx pid=%d tid=%ld image=%s "
            "libc=%s\n",
            (unsigned long long)m->id, getpid(), syscall(SYS_gettid),
            map ? map->l_name : path,
            libc.dli_fname ? libc.dli_fname : "UNKNOWN");

    void (*observe)(_Atomic int *) =
        (void *)dlsym(handle, "conformance_observe_destructor");
    if (observe) {
      observe(&G->destructors);
    }
    void (*dep)(_Atomic int *, _Atomic int *) =
        (void *)dlsym(handle, "conformance_observe_dependency");
    if (dep) {
      dep(&G->dependency_constructors, &G->dependency_destructors);
    }
    atomic_fetch_add(&G->constructors, 1);

    jint (*load)(JavaVM *, void *) = (void *)dlsym(handle, "JNI_OnLoad");
    /* OnLoad may call back into Java and re-cross the bridge; counting that
       nesting in gdepth selects the matching stable frame on both sides. */
    gdepth++;
    cmd->result.i = load ? load(&fg_vm, NULL) : JNI_VERSION_1_1;
    gdepth--;
  } else {
    if (m->id != cmd->module) {
      cmd->error = 8;
      goto done;
    }

    void *handle = m->handle;
    if (cmd->op == FB_COMMIT_LOAD) {
      if (handle && !m->committed) {
        m->committed = 1;
        atomic_fetch_add(&G->loads, 1);
      } else {
        cmd->error = 6;
      }

    } else if (cmd->op == FB_PREPARE_UNLOAD) {
      if (!handle || !m->committed || m->unload_prepared) {
        cmd->error = 11;
        goto done;
      }
      /* Invoke OnUnload while the image stays mapped. Host unregisters VM
         entries before allowing the separate final dlclose command. */
      m->unload_prepared = 1;
      void (*unload)(JavaVM *, void *) = (void *)dlsym(handle, "JNI_OnUnload");
      gdepth++;
      if (unload) {
        unload(&fg_vm, NULL);
      }
      gdepth--;
    } else if (cmd->op == FB_ABORT_LOAD || cmd->op == FB_UNLOAD) {
      /* Abort never runs OnUnload. The new Host prepares unload separately;
         the inline path only supports the legacy command ordering. A failed
         dlclose retains this record and makes the Host quarantine the bridge.
       */
      if (cmd->op == FB_UNLOAD && handle && !m->unload_prepared) {
        void (*unload)(JavaVM *, void *) =
            (void *)dlsym(handle, "JNI_OnUnload");
        gdepth++;
        if (unload) {
          unload(&fg_vm, NULL);
        }
        gdepth--;
      }

      if (handle) {
        int rc = dlclose(handle);
        if (!rc) {
          atomic_fetch_add(&G->image_closes, 1);
        } else {
          cmd->error = 12;
          snprintf(cmd->diagnostic, sizeof cmd->diagnostic, "%s", dlerror());
          goto done; /* Mapping state is uncertain: Host must quarantine. */
        }
        dprintf(2, "GUEST_IMAGE_EVENT phase=close handle=%llx rc=%d\n",
                (unsigned long long)m->id, rc);
      }

      pthread_mutex_lock(&modules_lock);
      memset(m, 0, sizeof *m);
      pthread_mutex_unlock(&modules_lock);
    } else if (cmd->op == FB_LOOKUP) {
      cmd->result.j = handle ? (jlong)(uintptr_t)dlsym(handle, cmd->sig) : 0;

    } else if (cmd->op == FB_NATIVE) {
      /* Call the guest function through libffi with the native-method
         calling convention: args[0] is the receiver (jclass or jobject) and
         args[1..] the declared parameters, mirroring the Host trampoline in
         ffi_native. The ffi cif is rebuilt per call; the Host caches its
         counterpart in the binding. */
      if (!handle) {
        cmd->error = 9;
        goto done;
      }

      GW_RPC_SCOPE(prepare, GW_RPC_GUEST_NATIVE_PREPARE, -1);
      char types[FB_ARGS], ret;
      int n;
      if (!sig_types(cmd->sig, types, &n, &ret)) {
        cmd->error = 4;
        GW_RPC_SCOPE_END(prepare);
        goto done;
      }

      ffi_type *ft[FB_ARGS + 2] = {&ffi_type_pointer, &ffi_type_pointer};
      void *args[FB_ARGS + 2];
      JNIEnv *env = &fg_env;
      args[0] = &env;
      args[1] = &cmd->args[0];
      for (int i = 0; i < n; i++) {
        ft[i + 2] = jni_ffi_type(types[i]);
        args[i + 2] = &cmd->args[i + 1];
      }

      ffi_cif cif;
      if (ffi_prep_cif(&cif, FFI_DEFAULT_ABI, n + 2, jni_ffi_type(ret), ft) !=
          FFI_OK) {
        cmd->error = 5;
        GW_RPC_SCOPE_END(prepare);
        goto done;
      }

      gdepth++;
      /* Track the deepest bridge nesting reached; diagnostic only. */
      int old = atomic_load(&G->max_depth);
      while (old < gdepth &&
             !atomic_compare_exchange_weak(&G->max_depth, &old, gdepth)) {
      }
      GW_RPC_SCOPE_END(prepare);

      GW_RPC_SCOPE(execute, GW_RPC_GUEST_NATIVE_EXECUTE, -1);
      ffi_call(&cif, (void (*)(void))cmd->function, &cmd->result, args);
      GW_RPC_SCOPE_END(execute);
      gdepth--;
      atomic_fetch_add(&G->native_calls, 1);
    } else {
      cmd->error = 10;
    }
  }

done:
  gmodule = previous;
  if (cmd->error) {
    atomic_fetch_add(&G->errors, 1);
  }

#if GW_RPC_PROFILE
  if (cmd->op == FB_NATIVE && handoff_depth < GW_HANDOFF_DEPTH) {
    gw_handoff_complete(handoff, handoff_depth);
  }
#endif

  /* The release publishes result/error/diagnostic before the wake below can
     make the Host observe the completed sequence number. */
  atomic_store_explicit(&cmd->response, seq, memory_order_release);
#ifdef GW_EXPERIMENT_DOMAIN_JNI
  extern int gw_experiment_guest_domain(void);
  if (!gw_experiment_guest_domain()) {
#endif
    fb_event_notify(&fb_context_at(G, gid)->host_event);
#ifdef GW_EXPERIMENT_DOMAIN_JNI
  }
#endif
}
