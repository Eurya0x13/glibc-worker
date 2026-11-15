#define _GNU_SOURCE
#include "host-runtime.h"
extern long launch_guest(unsigned long, void *, void *);
extern void domain_start(void);
struct jni_domain_session session;
static pthread_mutex_t owner_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local struct jni_domain_context *current;
static _Thread_local struct signal_slot *current_signal;
_Atomic int domains_created, domains_finished;
_Atomic int fail_next_context, expected_errors;
static _Thread_local int host_tls;

int adoption_signal_check(struct signal_slot *slot) {
  return slot->exchange->host_tls == (uintptr_t)&host_tls &&
         slot->exchange->host_tid == syscall(SYS_gettid);
}

int gw_experiment_host_domain(void) {
  return current && current->domain;
}

int gw_experiment_command(struct host_context *host, struct fb_call *call,
                          int sequence) {
  (void)host;
  if (!gw_experiment_host_domain()) {
    return 0;
  }
  struct exchange *e = &current->exchange;
  while (atomic_load_explicit(&call->response, memory_order_acquire) !=
         sequence) {
    e->swap(&e->host, &e->guest);
    full_host_poll();
  }
  return 1;
}

static void *stack_alloc(size_t size) {
  size_t page = (size_t)sysconf(_SC_PAGESIZE);
  char *p = mmap(NULL, size + 2 * page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS,
                 -1, 0);
  if (p == MAP_FAILED) {
    return NULL;
  }
  if (mprotect(p + page, size, PROT_READ | PROT_WRITE)) {
    munmap(p, size + 2 * page);
    return NULL;
  }
  return p + page;
}

int owner_command(struct jni_domain_context *context, int operation) {
  pthread_mutex_lock(&owner_lock);
  int seq = atomic_load(&session.request) + 1;
  session.context = context;
  session.operation = operation;
  atomic_store_explicit(&session.request, seq, memory_order_release);
  fb_event_notify(&fb_context_at(gw_host_bridge, 0)->guest_event);
  int observed;
  while ((observed = atomic_load_explicit(&session.response,
                                          memory_order_acquire)) != seq) {
    wait_word(&session.response, observed);
  }
  int error = session.error;
  pthread_mutex_unlock(&owner_lock);
  return error;
}

void start_owner(char **argv) {
  char pointer[32];
  snprintf(pointer, sizeof pointer, "%lx", (unsigned long)(uintptr_t)&session);
  char *arguments[] = {argv[1], "--library-path", argv[2], argv[3], pointer};
  struct gw_bootstrap_image image = gw_bootstrap_map(argv[1]);
  void *stack = stack_alloc(2 * 1024 * 1024);
  REQUIRE(stack);
  void *sp = gw_bootstrap_stack(image, stack, 2 * 1024 * 1024, arguments, 5);
  REQUIRE(launch_guest(CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
                           CLONE_THREAD | CLONE_SYSVSEM,
                       sp, image.entry) > 0);
  while (!atomic_load_explicit(&session.ready, memory_order_acquire)) {
    wait_word(&session.ready, 0);
  }
}

/* Called only after fh_context has reserved its stable production slot. */
int gw_experiment_context_start(struct host_context *host) {
  struct fb_context *w = fb_context_at(gw_host_bridge, host->id);
  int injected = atomic_exchange(&fail_next_context, 0);
  if (injected) {
    atomic_fetch_add(&expected_errors, 1);
  }
  if (injected && !session.domain) {
    w->attach_result = JNI_ENOMEM;
    atomic_store_explicit(&w->state, FB_ATTACH_FAILED, memory_order_release);
    return 1;
  }
  if (!session.domain) {
    return 0;
  }
  struct jni_domain_context *c = calloc(1, sizeof *c);
  if (!c) {
    w->attach_result = JNI_ENOMEM;
    atomic_store_explicit(&w->state, FB_ATTACH_FAILED, memory_order_release);
    return 1;
  }
  REQUIRE(!current);
  current = c;
  c->id = host->id;
  c->domain = 1;
  c->fail_allocate = injected;
  struct exchange *e = &c->exchange;
  e->swap = domain_swap;
  e->host_tid = w->host_tid;
  e->host_tls = (uintptr_t)&host_tls;
  e->stack_low = (uintptr_t)stack_alloc(2 * 1024 * 1024);
  if (!e->stack_low) {
    goto fail;
  }
  e->stack_high = e->stack_low + 2 * 1024 * 1024;
  current_signal = signal_acquire(e);
  if (!current_signal || owner_command(c, JNI_DOMAIN_ALLOC)) {
    goto fail;
  }
  e->guest.pc = (uintptr_t)domain_start;
#if defined(__aarch64__)
  e->guest.saved[0] = (uintptr_t)c;
  e->guest.saved[1] = (uintptr_t)session.entry;
#else
  e->guest.saved[2] = (uintptr_t)c;
  e->guest.saved[3] = (uintptr_t)session.entry;
  __asm__ volatile("stmxcsr %0" : "=m"(e->guest.saved[6]));
  __asm__ volatile("fnstcw %0" : "=m"(e->guest.saved[7]));
#endif
  e->swap(&e->host, &e->guest);
  REQUIRE(atomic_load(&c->ready));
  atomic_fetch_add(&domains_created, 1);
  atomic_store_explicit(&w->state, FB_RUNNING, memory_order_release);
  return 1;
fail:
  if (current_signal) {
    signal_release(current_signal);
  }
  current_signal = NULL;
  if (e->stack_low) {
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    munmap((void *)(e->stack_low - page), 2 * 1024 * 1024 + 2 * page);
  }
  free(c);
  current = NULL;
  w->attach_result = JNI_ENOMEM;
  atomic_store_explicit(&w->state, FB_ATTACH_FAILED, memory_order_release);
  return 1;
}

int gw_experiment_context_stop(struct host_context *host) {
  if (!current) {
    return 0;
  }
  struct jni_domain_context *c = current;
  REQUIRE(c->id == host->id);
  atomic_store(&c->stop, 1);
  c->exchange.swap(&c->exchange.host, &c->exchange.guest);
  REQUIRE(c->exchange.state == DOMAIN_DONE);
  REQUIRE(!owner_command(c, JNI_DOMAIN_FREE));
  signal_release(current_signal);
  current_signal = NULL;
  size_t page = (size_t)sysconf(_SC_PAGESIZE);
  REQUIRE(!munmap((void *)(c->exchange.stack_low - page),
                  2 * 1024 * 1024 + 2 * page));
  free(c);
  current = NULL;
  atomic_fetch_add(&domains_finished, 1);
  atomic_store_explicit(&fb_context_at(gw_host_bridge, host->id)->state,
                        FB_JOINED, memory_order_release);
  return 1;
}
