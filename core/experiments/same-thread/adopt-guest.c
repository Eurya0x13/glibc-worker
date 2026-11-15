#define _GNU_SOURCE
#include "adopt.h"
#include <dlfcn.h>
#include <errno.h>
#include <fenv.h>
#include <pthread.h>
#include <stdlib.h>

static struct adoption *control;
static int (*allocate_domain)(void *, size_t, void **, void **, void **);
static int (*bind_domain)(void *);
static int (*run_domain)(void (*)(void *), void *);
static int (*finish_domain)(void);
static int (*free_domain)(void *);
static int (*reserve_signals)(uint64_t);
static pthread_key_t key;
static pthread_once_t probe_once = PTHREAD_ONCE_INIT;
static domain_probe probe;
static __thread int guest_tls = 41;
extern int __cxa_thread_atexit_impl(void (*)(void *), void *, void *);
extern void *__dso_handle;

static void tls_destruct(void *value) {
  struct carrier *c = value;
  CHECK(&c->exchange, guest_tls == 41);
  ++c->tls_destructors;
}

static void load_probe(void) {
  void *handle = dlopen(control->dso, RTLD_NOW | RTLD_LOCAL);
  if (!handle || !(probe = (domain_probe)dlsym(handle, "domain_probe_call"))) {
    _exit(92);
  }
}

static void destruct(void *value) {
  struct carrier *c = value;
  ++c->guest_destructors;
}

static void cleanup(void *value) {
  struct carrier *c = value;
  ++c->cleanup_handlers;
}

static void *background(void *value) {
  struct carrier *c = value;
  if (control->note_guest_thread) {
    control->note_guest_thread(c->index + 1, syscall(SYS_gettid),
                               (uintptr_t)__builtin_thread_pointer());
  }
  c->background_ok = guest_tls == 41;
  guest_tls = 99;
  return value;
}

static void execute(struct exchange *e);

static void check_signal_ownership(struct exchange *e) {
  struct sigaction action = {.sa_handler = SIG_IGN}, previous;
  for (int number = 1; number <= 64; ++number) {
    if (!(control->host_signals & (UINT64_C(1) << (number - 1)))) {
      continue;
    }
    CHECK(e, sigaction(number, &action, NULL) == -1 && errno == EBUSY);
    CHECK(e, signal(number, SIG_IGN) == SIG_ERR && errno == EBUSY);
    /* Queries fail explicitly rather than returning a callable Host pointer. */
    CHECK(e, sigaction(number, NULL, &previous) == -1 && errno == EBUSY);
  }
  /* Calling the setup API again cannot release ownership. */
  CHECK(e, reserve_signals(0) == 0);
  if (control->host_signals) {
    CHECK(e, sigaction(SIGSEGV, &action, NULL) == -1 && errno == EBUSY);
    CHECK(e, reserve_signals(UINT64_C(1) << (SIGKILL - 1)) == EINVAL);
    CHECK(e, sigaction(SIGUSR1, NULL, &previous) == 0);
  }
}

static int callback(struct exchange *e, int value) {
  e->value = value;
  e->state = DOMAIN_CALLBACK;
  e->swap(&e->guest, &e->host);
  while (e->state == DOMAIN_CALL) {
    execute(e);
    e->swap(&e->guest, &e->host);
  }
  CHECK(e, e->state == DOMAIN_DONE);
  return e->value;
}

static void execute(struct exchange *e) {
  int result = probe(e, callback);
  CHECK(e, guest_tls == 41 && errno == EBADF);
  e->value = result;
  e->state = DOMAIN_DONE;
}

static void serve(void *argument) {
  struct carrier *c = argument;
  struct exchange *e = &c->exchange;
  pthread_once(&probe_once, load_probe);
  CHECK(e, pthread_setspecific(key, c) == 0);
  CHECK(e, __cxa_thread_atexit_impl(tls_destruct, c, &__dso_handle) == 0);
  CHECK(e, pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL) == ENOTSUP);
  CHECK(e, pthread_cancel(pthread_self()) == ENOTSUP);
  check_signal_ownership(e);
#ifdef GW_ADOPT_SETXID_GUARD
  CHECK(e, setuid(getuid()) == -1 && errno == ENOTSUP);
#endif
  pthread_t thread;
  int error = pthread_create(&thread, NULL, background, c);
  CHECK(e, error == 0);
  if (!error) {
    void *result;
    CHECK(e, pthread_join(thread, &result) == 0 && result == c);
    if (control->note_guest_thread) {
      control->note_guest_thread(c->index + 1, 0, 0);
    }
    CHECK(e, c->background_ok && guest_tls == 41);
  }
  errno = EBADF;
  if (e->host_signal_probe) {
    CHECK(e, raise(SIGUSR2) == 0);
    CHECK(e, raise(SIGPIPE) == 0);
    CHECK(e, raise(SIGXFSZ) == 0);
    for (int i = 0; i < 3; ++i) {
      CHECK(e, e->host_signal_delivered[i] == e->host_signal_expected[i]);
    }
    CHECK(e, errno == EBADF && guest_tls == 41);
  }
#ifdef GW_DOMAIN_SIGNALS
  CHECK(e, raise(DOMAIN_NOTIFY_SIGNAL) == 0);
  e->fault(e->fault_page);
  CHECK(e, errno == EBADF && guest_tls == 41);
#endif
  e->state = DOMAIN_READY;
  pthread_cleanup_push(cleanup, c);
  e->swap(&e->guest, &e->host);
  while (e->state != ADOPT_STOP) {
    CHECK(e, e->state == DOMAIN_CALL);
    execute(e);
    e->swap(&e->guest, &e->host);
  }
  if (c->exit_mode) {
    pthread_exit(c);
  }
  pthread_cleanup_pop(1);
}

static void adopted_entry(void *argument) {
  struct carrier *c = argument;
  struct exchange *e = &c->exchange;
  CHECK(e, bind_domain(c->token) == 0);
  CHECK(e, bind_domain(c->token) == EINVAL);
  e->guest_tid = syscall(SYS_gettid);
  e->guest_tls = (uintptr_t)&guest_tls;
  CHECK(e, guest_tls == 41 && fesetround(FE_UPWARD) == 0);
  c->run_status = run_domain(serve, c);
  CHECK(e, c->run_status == c->exit_mode);
  CHECK(e, finish_domain() == 0);
  e->state = ADOPT_STOPPED;
  e->swap(&e->guest, &e->host);
  _exit(93);
}

static void resolve_api(void) {
#define RESOLVE(variable, symbol)                      \
  do {                                                 \
    *(void **)&variable = dlsym(RTLD_DEFAULT, symbol); \
    if (!variable) {                                   \
      _exit(94);                                       \
    }                                                  \
  } while (0)
  RESOLVE(allocate_domain, "__gw_domain_alloc");
  RESOLVE(bind_domain, "__gw_domain_bind");
  RESOLVE(run_domain, "__gw_domain_run");
  RESOLVE(finish_domain, "__gw_domain_finish");
  RESOLVE(free_domain, "__gw_domain_free");
  RESOLVE(reserve_signals, "__gw_domain_reserve_signals");
#undef RESOLVE
}

int main(int argc, char **argv) {
  if (argc != 2) {
    _exit(91);
  }
  control = (void *)(uintptr_t)strtoull(argv[1], NULL, 16);
  if (control->note_guest_thread) {
    control->note_guest_thread(0, syscall(SYS_gettid),
                               (uintptr_t)__builtin_thread_pointer());
  }
  resolve_api();
  if (reserve_signals(control->host_signals)) {
    _exit(109);
  }
  if (pthread_key_create(&key, destruct)) {
    _exit(95);
  }
  control->entry = adopted_entry;
  atomic_store_explicit(&control->ready, 1, memory_order_release);
  adopt_wake(&control->ready);
  int sequence = 0;
  for (;;) {
    int request = atomic_load_explicit(&control->request, memory_order_acquire);
    if (request == sequence) {
      adopt_wait(&control->request, sequence);
      continue;
    }
    struct carrier *c = control->carrier;
    struct exchange *e = &c->exchange;
    if (control->operation == ADOPT_ALLOC) {
      void *tp = NULL, *top = NULL;
      control->error =
          allocate_domain((void *)e->stack_low, e->stack_high - e->stack_low,
                          &c->token, &tp, &top);
      e->guest.tp = (uintptr_t)tp;
      e->guest.sp = (uintptr_t)top & ~(uintptr_t)15;
    } else if (control->operation == ADOPT_FREE) {
      control->error = free_domain(c->token);
    } else {
      control->error = EINVAL;
    }
    sequence = request;
    atomic_store_explicit(&control->response, sequence, memory_order_release);
    adopt_wake(&control->response);
  }
}
