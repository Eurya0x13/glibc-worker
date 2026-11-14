#ifndef GW_HOST_INTERNAL_H
#define GW_HOST_INTERNAL_H
#include <jni.h>
#include <ffi.h>
#include <pthread.h>
#include "full_bridge.h"
#include "loader_backend_internal.h"
#include "jni_capabilities.h"
#pragma GCC visibility push(hidden)

/* Internal Host-only state. Never included by the GNU Guest. */
enum { H_LOCAL = 1, H_GLOBAL, H_WEAK, H_METHOD, H_FIELD };

struct host_context {
  JNIEnv *env;
  int id, depth, local_depth, critical, command_error;
  uint64_t module;
  char diagnostic[512];
};

struct binding {
  ffi_cif cif;
  ffi_type *args[FB_ARGS + 2];
  ffi_closure *closure;
  void *code;
  uintptr_t guest;
  uint64_t module;
  char sig[FB_SIG], types[FB_ARGS], ret;
  int nargs;
  _Atomic int active;
};
extern struct full_bridge *gw_host_bridge;
extern JavaVM *gw_host_vm;
extern jint gw_host_jni_version;
extern __thread struct host_context *gw_host_current;
extern jclass gw_host_helper_class;
extern jmethodID gw_host_describe_method, gw_host_declaring_method;
jobject fh_add_ref(struct host_context *, jobject, int);
jobject fh_add_id(struct host_context *, void *, int, const char *, jclass);
jobject fh_ref(struct host_context *, jobject);
void *fh_id(jobject, int);
void fh_drop_token(jobject);
void fh_drop_locals(struct host_context *, int);
void sweep_ids(JNIEnv *, int);
jclass fh_declaring_class(struct host_context *, jobject);
const char *fh_describe(struct host_context *, jobject);
int fh_decode(struct host_context *, jobject, jobject, char, jvalue *);
void fh_service(struct host_context *, struct fb_rpc *);
struct host_context *fh_context(JNIEnv *);
jvalue fh_command(struct host_context *, int, uintptr_t, const char *,
                  jvalue *);
struct binding *make_binding(uint64_t, uintptr_t, const char *,
                             struct gw_loader_error *);
void release_bindings(uint64_t);
void fh_bindings_finish(void);
const char *fh_binding_backend(void);
int release_registrations(JNIEnv *, uint64_t);
jint fh_transport_load(JNIEnv *, uint64_t, const char *,
                       struct gw_loader_error *);
int fh_transport_close(JNIEnv *, uint64_t, struct gw_loader_error *);
int fh_transport_find(JNIEnv *, uint64_t, const char *, const char *, void **,
                      struct gw_loader_error *);
jint fh_register(struct host_context *, jclass, const JNINativeMethod *, jint);
void fh_array_copy_start(int);
void fh_array_copy_finish(void);
// clang-format off
#define ARRAY_COPY_DECL(type) \
  j##type *fh_get_##type##_elements(JNIEnv *, j##type##Array, jboolean *); \
  void fh_release_##type##_elements(JNIEnv *, j##type##Array, \
                                    j##type *, jint);
// clang-format on

ARRAY_COPY_DECL(boolean)
ARRAY_COPY_DECL(byte)
ARRAY_COPY_DECL(char)
ARRAY_COPY_DECL(short)
ARRAY_COPY_DECL(int)
ARRAY_COPY_DECL(long)
ARRAY_COPY_DECL(float)
ARRAY_COPY_DECL(double)
#undef ARRAY_COPY_DECL
#pragma GCC visibility pop
#endif
