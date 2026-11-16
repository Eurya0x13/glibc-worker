#define _GNU_SOURCE
#include "signal_profile.h"
#ifdef GW_SIGNAL_PROFILE_DISABLED
#include <errno.h>

int domain_late_signal_install(JNIEnv *env) {
  (void)env;
  return ENOTSUP;
}

int domain_late_signal_restore(void) {
  return ENOTSUP;
}
#else
#include "late-signal-profile.h"
#include "signal-router.h"
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

struct binding {
  uintptr_t *slot;
  uintptr_t original;
  uintptr_t replacement;
  int protection;
};
static struct binding bindings[3];
static unsigned installed, binding_count;
static void *chain_base;
static signal_router_backend vm_action;
static void *(*system_lookup)(void *, const char *);
static uintptr_t libc_action;

#ifndef GW_LATE_ART_ONLY
static void *vm_lookup(void *handle, const char *name) {
  void *result = system_lookup(handle, name);
  if (handle == RTLD_DEFAULT && !strcmp(name, "sigaction") &&
      (uintptr_t)result == libc_action) {
    return signal_router_action;
  }
  return result;
}

#endif

static size_t page_size;

static int verify_artifact(JNIEnv *env, const char *path, const char *sha256) {
  jclass helper = (*env)->FindClass(env, "dev/glibc/worker/JvmAdapter");
  if (!helper) {
    return 0;
  }
  jmethodID verify = (*env)->GetStaticMethodID(
      env, helper, "verifyLibrary", "(Ljava/lang/String;Ljava/lang/String;)Z");
  jstring file = verify ? (*env)->NewStringUTF(env, path) : NULL;
  jstring digest = file ? (*env)->NewStringUTF(env, sha256) : NULL;
  int matches = digest && (*env)->CallStaticBooleanMethod(env, helper, verify,
                                                          file, digest);
  int pending = (*env)->ExceptionCheck(env);
  if (digest) {
    (*env)->DeleteLocalRef(env, digest);
  }
  if (file) {
    (*env)->DeleteLocalRef(env, file);
  }
  (*env)->DeleteLocalRef(env, helper);
  return matches && !pending;
}

/* Pinned data pointers only, never instructions. The caller first checks the
 * exact owning image with dladdr, permitting its anonymous zero-filled BSS.
 * Preserve the actual RELRO permission on all paths. */
static int slot_protection(uintptr_t address, const struct stat *file) {
  FILE *maps = fopen("/proc/self/maps", "re");
  if (!maps) {
    return -1;
  }
  char *line = NULL;
  size_t capacity = 0;
  int protection = -1;
  while (getline(&line, &capacity, maps) != -1) {
    unsigned long begin, end, offset, inode;
    unsigned device_major, device_minor;
    char permissions[5];
    if (sscanf(line, "%lx-%lx %4s %lx %x:%x %lu", &begin, &end, permissions,
               &offset, &device_major, &device_minor, &inode) != 7 ||
        address < begin || address > end - sizeof(uintptr_t)) {
      continue;
    }
    if (permissions[0] == 'r' && permissions[2] != 'x' &&
        permissions[3] == 'p' &&
        ((inode == (unsigned long)file->st_ino &&
          device_major == major(file->st_dev) &&
          device_minor == minor(file->st_dev)) ||
         (inode == 0 && device_major == 0 && device_minor == 0))) {
      protection = PROT_READ | (permissions[1] == 'w' ? PROT_WRITE : 0);
    }
    break;
  }
  free(line);
  fclose(maps);
  return protection;
}

static int replace_import(struct binding *binding, uintptr_t expected,
                          uintptr_t replacement) {
  void *page = (void *)((uintptr_t)binding->slot & ~(page_size - 1));
  if (mprotect(page, page_size, binding->protection | PROT_WRITE)) {
    return errno;
  }
  int changed =
      __atomic_compare_exchange_n(binding->slot, &expected, replacement, 0,
                                  __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
  if (mprotect(page, page_size, binding->protection)) {
    /* A writable RELRO page is not a valid recoverable installation. This
     * disposable experiment cannot resume the application in that state. */
    _exit(109);
  }
  return changed ? 0 : EBUSY;
}

static int prepare_binding(unsigned index, const Dl_info *owner,
                           const struct stat *file, uintptr_t offset,
                           uintptr_t expected, uintptr_t replacement,
                           int allow_empty) {
  uintptr_t address = (uintptr_t)owner->dli_fbase + offset;
  Dl_info mapping = {0};
  if (address < (uintptr_t)owner->dli_fbase || address % _Alignof(uintptr_t) ||
      !dladdr((void *)address, &mapping) ||
      mapping.dli_fbase != owner->dli_fbase) {
    return ENOTSUP;
  }
  int protection = slot_protection(address, file);
  if (protection < 0) {
    return ENOTSUP;
  }
  uintptr_t value = __atomic_load_n((uintptr_t *)address, __ATOMIC_ACQUIRE);
  if (value != expected && !(allow_empty && !value)) {
    return ENOTSUP;
  }
  bindings[index] =
      (struct binding){(uintptr_t *)address, value, replacement, protection};
  return 0;
}

static int prepare_plain_vm(const Dl_info *origin, const struct stat *file) {
#ifdef GW_LATE_ART_ONLY
  /* Stripped Pojav artifact: only its reviewed ART path is supported. Never
   * fabricate a diagnostic-cache offset for standalone use. */
  (void)origin;
  (void)file;
  return ENOTSUP;
#else
  const uintptr_t offsets[] = {GW_LATE_DLSYM_OFFSET, GW_LATE_QUERY_CACHE_OFFSET,
                               GW_LATE_SIGACTION_OFFSET};
  const uintptr_t replacements[] = {(uintptr_t)vm_lookup,
                                    (uintptr_t)signal_router_action,
                                    (uintptr_t)signal_router_action};
  for (unsigned i = 0; i < 3; ++i) {
    int status =
        prepare_binding(i, origin, file, offsets[i],
                        i == 0 ? (uintptr_t)system_lookup : libc_action,
                        replacements[i], i == 1);
    if (status) {
      return status;
    }
  }
  binding_count = 3;
  vm_action = signal_router_action;
  return 0;
#endif
}

static int prepare_art_chain(JNIEnv *env, uintptr_t public_action, void *libc) {
#ifdef GW_ART_SIGCHAIN_SHA256
  Dl_info owner = {0}, ensure_owner = {0};
  struct stat file;
  void *front = dlsym(RTLD_DEFAULT, "EnsureFrontOfChain");
  if (!dladdr((void *)public_action, &owner) || !owner.dli_fname ||
      !strstr(owner.dli_fname, "libsigchain.so") || !front ||
      !dladdr(front, &ensure_owner) ||
      ensure_owner.dli_fbase != owner.dli_fbase ||
      dlsym(RTLD_DEFAULT, "sigaction") != (void *)public_action ||
      stat(owner.dli_fname, &file) ||
      !verify_artifact(env, owner.dli_fname, GW_ART_SIGCHAIN_SHA256)) {
    return ENOTSUP;
  }
  void *action64 = dlsym(libc, "sigaction64");
  if (!action64) {
    return ENOTSUP;
  }
  int status = prepare_binding(0, &owner, &file, GW_ART_SIGACTION_OFFSET,
                               libc_action, (uintptr_t)signal_router_action, 0);
  if (!status) {
    status = prepare_binding(1, &owner, &file, GW_ART_SIGACTION64_OFFSET,
                             (uintptr_t)action64,
                             (uintptr_t)signal_router_action64, 0);
  }
  if (!status) {
    chain_base = owner.dli_fbase;
    vm_action = (void *)public_action;
    binding_count = 2;
  }
  return status;
#else
  (void)env;
  (void)public_action;
  (void)libc;
  return ENOTSUP;
#endif
}

int domain_late_signal_install(JNIEnv *env) {
  if (installed) {
    return EALREADY;
  }
  const char *path = getenv("GW_JVM_LIB");
  if (!path) {
    return EINVAL;
  }
  void *jvm = dlopen(path, RTLD_NOW | RTLD_NOLOAD);
  void *create = jvm ? dlsym(jvm, "JNI_CreateJavaVM") : NULL;
  Dl_info origin = {0};
  struct stat expected, resident;
  if (!create || !dladdr(create, &origin) || !origin.dli_fname ||
      stat(path, &expected) || stat(origin.dli_fname, &resident) ||
      expected.st_dev != resident.st_dev ||
      expected.st_ino != resident.st_ino ||
      !verify_artifact(env, origin.dli_fname, GW_LATE_JVM_SHA256)) {
    return ENOEXEC;
  }
  /* Keep the explicit handle resident even on failure. An in-flight VM may
   * still be using it; this API never owns VM or DSO unloading. */
#ifdef __ANDROID__
  void *libc = dlopen("libc.so", RTLD_NOW | RTLD_LOCAL);
#else
  void *libc = dlopen("libc.so.6", RTLD_NOW | RTLD_LOCAL);
#endif
  void *backend = libc ? dlsym(libc, "sigaction") : NULL;
  system_lookup = dlsym;
  libc_action = (uintptr_t)backend;
  uintptr_t vm_slot = (uintptr_t)origin.dli_fbase + GW_LATE_SIGACTION_OFFSET;
  Dl_info slot_owner = {0};
  if (!backend || vm_slot < (uintptr_t)origin.dli_fbase ||
      !dladdr((void *)vm_slot, &slot_owner) ||
      slot_owner.dli_fbase != origin.dli_fbase ||
      slot_protection(vm_slot, &resident) < 0) {
    return ENOTSUP;
  }
  uintptr_t public_action =
      __atomic_load_n((uintptr_t *)vm_slot, __ATOMIC_ACQUIRE);
  int prepared = public_action == libc_action
                     ? prepare_plain_vm(&origin, &resident)
                     : prepare_art_chain(env, public_action, libc);
  if (prepared) {
    fprintf(stderr, "LATE_SIGNAL_GATE stage=prepare status=%d\n", prepared);
    return prepared;
  }
  long page = sysconf(_SC_PAGESIZE);
  if (page <= 0 || ((unsigned long)page & ((unsigned long)page - 1))) {
    return EINVAL;
  }
  page_size = (size_t)page;
  int status = signal_router_init_with_backend(backend);
  if (status) {
    return status;
  }
  struct sigaction before;
  if (vm_action(SIGSEGV, NULL, &before)) {
    return errno;
  }
  /* Route both normal calls and HotSpot's diagnostic lookup/cache before
   * publishing kernel wrappers. No VM check is disabled. The embedding must
   * quiesce application JNI calls/registration during this transaction. */
  for (unsigned i = 0; i < binding_count; ++i) {
    status = replace_import(&bindings[i], bindings[i].original,
                            bindings[i].replacement);
    if (status) {
      break;
    }
    ++installed;
    const char *inject = getenv("GW_LATE_FAIL_AFTER");
    if (inject && (unsigned)atoi(inject) == installed) {
      status =
          ENOMEM;  // Disposable fixture: exercise rollback after each write.
      break;
    }
  }
  if (!status) {
    status = signal_router_adopt_chain(chain_base);
  }
  if (status) {
    while (installed) {
      struct binding *binding = &bindings[--installed];
      if (replace_import(binding, binding->replacement, binding->original)) {
        _exit(108);
      }
    }
    if (signal_router_restore()) {
      _exit(108);
    }
    for (unsigned i = 0; i < binding_count; ++i) {
      if (__atomic_load_n(bindings[i].slot, __ATOMIC_ACQUIRE) !=
          bindings[i].original) {
        _exit(108);
      }
    }
    printf(
        "LATE_SIGNAL_ROLLBACK {\"result\":\"PASS\",\"status\":%d,"
        "\"bindings_restored\":%u}\n",
        status, binding_count);
    fflush(stdout);
    return status;
  }
  struct sigaction logical;
  /* Inspect the chain; never ask ART to reorder it as a validation step.
   * An existing outer handler (for example ByteHook) may already forward to
   * ART. EnsureFrontOfChain would insert that handler below ART as well,
   * creating ART -> outer handler -> ART recursion on the first VM fault. */
  if (vm_action(SIGSEGV, NULL, &logical) ||
      logical.sa_handler != before.sa_handler ||
      logical.sa_flags != before.sa_flags || signal_router_verify()) {
    _exit(108);
  }
  for (int number = 1; number < NSIG; ++number) {
    if (sigismember(&logical.sa_mask, number) !=
        sigismember(&before.sa_mask, number)) {
      _exit(108);
    }
  }
  printf(
      "LATE_SIGNAL_INSTALL {\"result\":\"PASS\",\"bindings\":%u,"
      "\"art_chain\":%s,\"adopted\":%u,\"global_interposition\":false}\n",
      binding_count, chain_base ? "true" : "false",
      signal_router_registrations());
  fflush(stdout);
  return 0;
}

int domain_late_signal_restore(void) {
  if (!installed || installed != binding_count) {
    return EINVAL;
  }
  /* Caller has stopped Guest execution and destroyed the VM. */
  int status = signal_router_verify();
  if (status) {
    return status;
  }
  for (unsigned i = 0; i < installed; ++i) {
    if (__atomic_load_n(bindings[i].slot, __ATOMIC_ACQUIRE) !=
        bindings[i].replacement) {
      return EBUSY;
    }
  }
  while (installed) {
    struct binding *binding = &bindings[installed - 1];
    status = replace_import(binding, binding->replacement, binding->original);
    if (status) {
      return status;
    }
    --installed;
  }
  return signal_router_restore();
}

#endif
