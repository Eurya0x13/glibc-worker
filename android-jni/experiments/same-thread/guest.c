#define _GNU_SOURCE
#include "transport.h"
#include "guest_internal.h"
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>

static struct jni_domain_session *session;
static __thread struct jni_domain_context *current;
static int (*install_thread_hook)(void (*)(int));
static int (*allocate_domain)(void *, size_t, void **, void **, void **);
static int (*bind_domain)(void *);
static int (*run_domain)(void (*)(void *), void *);
static int (*finish_domain)(void);
static int (*free_domain)(void *);
static int (*reserve_signals)(uint64_t);

int gw_experiment_guest_domain(void) {
  return current && current->domain;
}

static void yield_host(void) {
  struct exchange *e = &current->exchange;
  e->swap(&e->guest, &e->host);
}

int gw_experiment_rpc(struct fb_rpc *rpc, int sequence) {
  if (!gw_experiment_guest_domain()) {
    return 0;
  }
  while (atomic_load_explicit(&rpc->response, memory_order_acquire) !=
         sequence) {
    yield_host();
    fg_pump(); /* Java may have re-entered a deeper native call. */
  }
  return 1;
}

static void serve(void *argument) {
  struct jni_domain_context *c = argument;
  current = c;
  gid = c->id;
  gdepth = 0;
  gmodule = 0;
  struct fb_context *w = fb_context_at(G, gid);
  w->guest_tid = (int)syscall(SYS_gettid);
  atomic_store_explicit(&c->ready, 1, memory_order_release);
  wake_word(&c->ready);
  yield_host();
  while (!atomic_load(&c->stop)) {
    fg_pump();
    yield_host();
  }
  gid = -1;
  current = NULL;
}

static void adopted_entry(void *argument) {
  struct jni_domain_context *c = argument;
  if (bind_domain(c->token) || run_domain(serve, c) || finish_domain()) {
    _exit(121);
  }
  c->exchange.state = DOMAIN_DONE;
  c->exchange.swap(&c->exchange.guest, &c->exchange.host);
  _exit(122);
}

static void resolve(void) {
#define API(variable, name)                          \
  do {                                               \
    *(void **)&variable = dlsym(RTLD_DEFAULT, name); \
    if (!variable)                                   \
      _exit(123);                                    \
  } while (0)
  API(install_thread_hook, "__gw_domain_thread_hook");
  API(allocate_domain, "__gw_domain_alloc");
  API(bind_domain, "__gw_domain_bind");
  API(run_domain, "__gw_domain_run");
  API(finish_domain, "__gw_domain_finish");
  API(free_domain, "__gw_domain_free");
  API(reserve_signals, "__gw_domain_reserve_signals");
#undef API
}

int main(int argc, char **argv) {
  if (argc != 2) {
    _exit(124);
  }
  session = (void *)(uintptr_t)strtoull(argv[1], NULL, 16);
  session->note_guest(0, syscall(SYS_gettid),
                      (uintptr_t)__builtin_thread_pointer());
  resolve();
  if (install_thread_hook(session->guest_thread)) {
    _exit(125);
  }
  if (reserve_signals(session->reserved_signals)) {
    _exit(125);
  }
  full_guest_start(&session->shared);
  session->entry = adopted_entry;
  atomic_store_explicit(&session->shared.phase, 2, memory_order_release);
  atomic_store_explicit(&session->ready, 1, memory_order_release);
  wake_word(&session->ready);
  int sequence = 0;
  for (;;) {
    struct fb_event *event = &fb_context_at(G, 0)->guest_event;
    unsigned observed = fb_event_sample(event);
    int request = atomic_load_explicit(&session->request, memory_order_acquire);
    if (request != sequence) {
      struct jni_domain_context *c = session->context;
      session->error = 0;
      if (session->operation == JNI_DOMAIN_ALLOC) {
        void *tp, *top;
        session->error = c->fail_allocate
                             ? ENOMEM
                             : allocate_domain((void *)c->exchange.stack_low,
                                               c->exchange.stack_high -
                                                   c->exchange.stack_low,
                                               &c->token, &tp, &top);
        if (!session->error) {
          c->exchange.guest.tp = (uintptr_t)tp;
          c->exchange.guest.sp = (uintptr_t)top & ~(uintptr_t)15;
        }
      } else if (session->operation == JNI_DOMAIN_FREE) {
        session->error = free_domain(c->token);
      } else if (session->operation == JNI_DOMAIN_STOP) {
        full_guest_shutdown();
      } else {
        session->error = EINVAL;
      }
      sequence = request;
      atomic_store_explicit(&session->response, sequence, memory_order_release);
      wake_word(&session->response);
    }
    full_guest_poll();
    fb_event_wait(event, observed);
  }
}
