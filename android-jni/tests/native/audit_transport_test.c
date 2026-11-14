/* Real Host command packing and loader transaction code; VM/Guest execution
   are test doubles. Failure cases run in separate child processes because
   production quarantine is irreversible. */
#define _GNU_SOURCE
#include <assert.h>
#include <limits.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "host_internal.h"
#include "dlopen_backend.h"

static struct full_bridge bridge;
struct full_bridge *gw_host_bridge = &bridge;
jint gw_host_jni_version = JNI_VERSION_1_8;
static struct host_context context;
static int pending, pushed, popped, dropped, released, swept, fail_unregister;
static int fail_command, reject_load, throw_load, throw_prepare, throw_close;
static jint load_version = JNI_VERSION_1_8;
static int operations[32], operation_count, cleanup[32], cleanup_count;
static const char *exception_type;
static struct binding binding;
static int fail_pages;
static _Atomic int stall, published, returned;
static int monitor_fd, lose_reply;
void *__real_mmap(void *, size_t, int, int, int, off_t);

void *__wrap_mmap(void *p, size_t n, int prot, int flags, int fd, off_t off) {
  if (fail_pages) {
    errno = ENOMEM;
    return MAP_FAILED;
  }
  return __real_mmap(p, n, prot, flags, fd, off);
}

static jint push_frame(JNIEnv *env, jint capacity) {
  (void)env;
  (void)capacity;
  pushed++;
  return JNI_OK;
}

static jobject pop_frame(JNIEnv *env, jobject result) {
  (void)env;
  popped++;
  return result;
}

static jboolean exception_check(JNIEnv *env) {
  (void)env;
  return pending != 0;
}

static jthrowable exception_occurred(JNIEnv *env) {
  (void)env;
  return (jthrowable)(uintptr_t)pending;
}

static void exception_clear(JNIEnv *env) {
  (void)env;
  pending = 0;
}

static jint throw_exception(JNIEnv *env, jthrowable value) {
  (void)env;
  pending = (int)(uintptr_t)value;
  return JNI_OK;
}

static jclass find_class(JNIEnv *env, const char *name) {
  (void)env;
  exception_type = name;
  return (jclass)(uintptr_t)1;
}

static jint throw_new(JNIEnv *env, jclass cls, const char *message) {
  (void)env;
  (void)cls;
  (void)message;
  pending = 1;
  return JNI_OK;
}

static void delete_local(JNIEnv *env, jobject object) {
  (void)env;
  (void)object;
}
static const struct JNINativeInterface_ vm = {
    .PushLocalFrame = push_frame,
    .PopLocalFrame = pop_frame,
    .ExceptionCheck = exception_check,
    .ExceptionOccurred = exception_occurred,
    .ExceptionClear = exception_clear,
    .Throw = throw_exception,
    .FindClass = find_class,
    .ThrowNew = throw_new,
    .DeleteLocalRef = delete_local};
static JNIEnv environment = &vm;

struct host_context *fh_context(JNIEnv *env, struct gw_loader_error *error) {
  (void)error;
  context.env = env;
  return &context;
}

int gw_loader_fail(struct gw_loader_error *error, int code, const char *text) {
  return gw_dlopen_fail(error, code, text);
}

void fh_drop_locals(struct host_context *c, int depth) {
  (void)c;
  assert(depth == 1);
  dropped++;
}

void release_bindings(uint64_t id) {
  assert(id == 17);
  released++;
  cleanup[cleanup_count++] = 101;
}

void sweep_ids(JNIEnv *env, int all) {
  (void)env;
  assert(!all && !pending);
  swept++;
}

int release_registrations(JNIEnv *env, uint64_t id) {
  (void)env;
  assert(id == 17 && !pending);
  cleanup[cleanup_count++] = 100;
  if (fail_unregister) {
    pending = 88;
    return JNI_ERR;
  }
  return JNI_OK;
}

struct binding *make_binding(uint64_t id, uintptr_t guest, const char *sig,
                             struct gw_loader_error *error) {
  (void)error;
  assert(id == 17 && guest == 31 && !strcmp(sig, "()I"));
  binding.code = (void *)(uintptr_t)32;
  return &binding;
}

void full_host_poll(void) {
  struct fb_call *command = fb_call_at(&bridge.ctx[0], context.depth - 1);
  int sequence = atomic_load_explicit(&command->request, memory_order_acquire);
  if (atomic_load(&stall)) {
    atomic_store_explicit(&published, 1, memory_order_release);
    return;
  }
  assert(sequence != atomic_load(&command->response));
  operations[operation_count++] = command->op;
  cleanup[cleanup_count++] = command->op;
  if (command->op == FB_LOAD) {
    command->result.i = reject_load ? (jint)0x00190000 : load_version;
    if (throw_load) {
      pending = 42;
    }
  }
  if (command->op == FB_LOOKUP) {
    command->result.j = 31;
  }
  if (command->op == FB_PREPARE_UNLOAD && throw_prepare) {
    pending = 77;
  }
  if (command->op == FB_UNLOAD && throw_close) {
    pending = 99;
  }
  if (command->op == fail_command) {
    command->error = 9;
    snprintf(command->diagnostic, sizeof command->diagnostic,
             "controlled Guest failure");
  }
  atomic_store_explicit(&command->response, sequence, memory_order_release);
}

static void scenario(unsigned test) {
  struct gw_loader_error error = {0};
  context.env = &environment;
  if (test == 0) {
    assert(fh_transport_load(&environment, 17, "image", &error) ==
           JNI_VERSION_1_8);
    assert(operation_count == 2 && operations[0] == FB_LOAD &&
           operations[1] == FB_COMMIT_LOAD);
    assert(!gw_dlopen_is_quarantined() && !released);
  } else if (test == 1 || test == 2) {
    reject_load = throw_load = 1;
    fail_unregister = test == 2;
    assert(fh_transport_load(&environment, 17, "image", &error) == JNI_ERR);
    assert(pending == 42); /* Cleanup failure must not replace original. */
    assert(operation_count == (test == 1 ? 2 : 1));
    assert(released == (test == 1 ? 1 : 0));
    assert(gw_dlopen_is_quarantined() == (test == 2));
  } else if (test >= 3 && test <= 6) {
    fail_unregister = test == 4;
    fail_command = test == 5 ? FB_PREPARE_UNLOAD : test == 6 ? FB_UNLOAD : 0;
    throw_prepare = throw_close = 1;
    int rc = fh_transport_close(&environment, 17, &error);
    assert(pending == 77);
    assert(rc == (test == 3 ? GW_OK : GW_ERROR));
    assert(released == (test == 3 ? 1 : 0));
    assert(operations[0] == FB_PREPARE_UNLOAD);
    assert(operation_count == (test == 3 || test == 6 ? 2 : 1));
    if (test == 3) {
      assert(cleanup_count == 4 && cleanup[0] == FB_PREPARE_UNLOAD &&
             cleanup[1] == 100 && cleanup[2] == FB_UNLOAD && cleanup[3] == 101);
    }
  } else if (test == 7 || test == 8) {
    reject_load = 1;
    fail_command = FB_ABORT_LOAD;
    if (test == 8) {
      reject_load = 0;
      fail_command = FB_COMMIT_LOAD;
    }
    assert(fh_transport_load(&environment, 17, "image", &error) == JNI_ERR);
    assert(!released && gw_dlopen_is_quarantined());
  } else if (test == 9) {
    char long_sig[FB_SIG + 1];
    memset(long_sig, 'I', FB_SIG);
    long_sig[FB_SIG] = 0;
    struct fb_call *slot = &bridge.ctx[0].call[0];
    fh_command(&context, FB_NATIVE, 0, long_sig, NULL);
    assert(context.command_error == GW_E_ARGUMENT &&
           !strcmp(exception_type, "java/lang/IllegalArgumentException"));
    assert(!atomic_load(&slot->request) && !operation_count && !context.depth);
    pending = 0;
    context.depth = FB_DEPTH;
    fail_pages = 1;
    fh_command(&context, FB_NATIVE, 0, "()I", NULL);
    assert(context.command_error == GW_E_CAPACITY &&
           !strcmp(exception_type, "java/lang/OutOfMemoryError"));
    fail_pages = 0;
    context.depth = pending = 0;
    context.emergency = 1;
    fh_command(&context, FB_NATIVE, 0, "()I", NULL);
    assert(context.command_error == GW_E_CAPACITY &&
           !atomic_load(&slot->request));
    context.emergency = pending = 0;
    atomic_store(&slot->request, INT_MAX);
    atomic_store(&slot->response, INT_MAX);
    strcpy(slot->sig, "stale-signature");
    fh_command(&context, FB_OWNER, 0, NULL, NULL);
    assert(atomic_load(&slot->response) == INT_MIN && !slot->sig[0]);
    return;
  } else if (test == 11) {
    load_version = 0x00090000; /* Host negotiated 1.8, Guest returned JNI9. */
    assert(fh_transport_load(&environment, 17, "image", &error) == JNI_ERR);
    assert(!gw_dlopen_is_quarantined() && released == 1 &&
           error.code == GW_E_VERSION);
    return;
  } else if (test == 12) {
    struct fb_call *hello = &bridge.ctx[0].call[0];
    assert(!fb_loader_acknowledge(hello) && !fb_loader_accepted(hello));
    fb_loader_offer(hello);
    assert(!fb_loader_accepted(hello) && fb_loader_acknowledge(hello) &&
           fb_loader_accepted(hello));
    for (int field = 14; field <= 23; field++) {
      if (field == 15) {
        continue;
      }
      fb_loader_offer(hello);
      hello->args[field].j++;
      assert(!fb_loader_acknowledge(hello) && !fb_loader_accepted(hello));
    }
    return;
  } else {
    assert(test == 10);
    void *entry = NULL;
    fail_command = FB_LOOKUP;
    assert(fh_transport_find(&environment, 17, "symbol", "()I", &entry,
                             &error) == GW_ERROR);
    assert(error.code == GW_E_BRIDGE && !entry);
    return;
  }
  assert(pushed == 1 && popped == 1 && dropped == 1 && !context.module &&
         !context.local_depth);
  if (gw_dlopen_is_quarantined()) {
    assert(error.code == GW_E_QUARANTINED &&
           strstr(error.message, "discard process"));
    int before = operation_count;
    pending = 0;
    assert(fh_transport_close(&environment, 17, &error) == GW_ERROR);
    assert(operation_count == before);
    assert(gw_dlopen_stop(&error) == GW_ERROR &&
           error.code == GW_E_QUARANTINED);
  }
}

/* Watchdog runs outside the blocked caller. Quarantine is a refusal gate,
   never an acknowledgement that the Guest stopped using borrowed memory. */
static void *watch_pending(void *unused) {
  (void)unused;
  while (!atomic_load_explicit(&published, memory_order_acquire)) {
    sched_yield();
  }
  struct fb_call *call = &bridge.ctx[0].call[0];
  int sequence = atomic_load_explicit(&call->request, memory_order_acquire);
  assert(sequence != atomic_load(&call->response));
  gw_dlopen_quarantine();
  fb_event_notify(&bridge.ctx[0].host_event);
  usleep(25000); /* Test observation window, not a production deadline. */
  assert(!atomic_load(&returned) && context.depth == 1);
  assert(sequence != atomic_load(&call->response));
  int *borrowed = (int *)call->args[0].l;
  assert(*borrowed == 73 && !released && !dropped && !popped);
  assert(write(monitor_fd, "P", 1) == 1);
  if (!lose_reply) {
    *borrowed = 94;
    call->result.i = *borrowed;
    atomic_store_explicit(&call->response, sequence, memory_order_release);
    fb_event_notify(&bridge.ctx[0].host_event);
  }
  return NULL;
}

static void pending_case(int lost) {
  int channel[2];
  assert(!pipe(channel));
  pid_t pid = fork();
  assert(pid >= 0);
  if (!pid) {
    close(channel[0]);
    monitor_fd = channel[1];
    lose_reply = lost;
    context.env = &environment;
    int borrowed = 73;
    jvalue args[FB_ARGS + 2] = {{.l = (jobject)&borrowed}};
    atomic_store(&stall, 1);
    pthread_t watcher;
    assert(!pthread_create(&watcher, NULL, watch_pending, NULL));
    jvalue result = fh_command(&context, FB_NATIVE, 0, "()I", args);
    atomic_store(&returned, 1);
    assert(!lost && result.i == 94 && borrowed == 94 && !context.depth);
    assert(!pthread_join(watcher, NULL));
    assert(gw_dlopen_is_quarantined());
    fh_command(&context, FB_NATIVE, 0, "()I", NULL);
    assert(context.command_error == GW_E_QUARANTINED);
    assert(write(monitor_fd, "D", 1) == 1);
    _exit(0);
  }
  close(channel[1]);
  struct pollfd event = {.fd = channel[0], .events = POLLIN};
  char stage = 0;
  int ready = poll(&event, 1, 5000);
  int observed = ready > 0 && read(channel[0], &stage, 1) == 1 && stage == 'P';
  if (lost || !observed) {
    /* Only the test supervisor terminates its disposable child process. */
    assert(!kill(pid, SIGKILL));
  } else {
    ready = poll(&event, 1, 5000);
    if (ready <= 0 || read(channel[0], &stage, 1) != 1 || stage != 'D') {
      kill(pid, SIGKILL);
      observed = 0;
    }
  }
  int status;
  assert(waitpid(pid, &status, 0) == pid);
  close(channel[0]);
  assert(observed);
  if (lost) {
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
  } else {
    assert(WIFEXITED(status) && !WEXITSTATUS(status));
  }
}

int main(void) {
  for (unsigned test = 0; test < 13; test++) {
    pid_t pid = fork();
    assert(pid >= 0);
    if (!pid) {
      scenario(test);
      _exit(0);
    }
    int status;
    assert(waitpid(pid, &status, 0) == pid);
    if (!WIFEXITED(status) || WEXITSTATUS(status)) {
      fprintf(stderr, "scenario %u failed\n", test);
      abort();
    }
  }
  pending_case(0);
  pending_case(1);
  puts(
      "{\"result\":\"PASS\",\"scenarios\":15,\"cleanup_order\":true,"
      "\"quarantine\":true,\"exception_preserved\":true,"
      "\"pending_call_retained\":true,\"late_reply\":true,"
      "\"supervisor_disposal\":true}");
}
