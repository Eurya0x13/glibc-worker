/* Isolate a retained backend's borrow/scope bookkeeping. Fake transport,
   actual exported functions and pthread mutexes; no JNI, Guest or RPC. */
#define _GNU_SOURCE
#include "dlopen_backend.h"
#include <assert.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

static int (*enter)(uint64_t), (*scope_leave)(uint64_t);
static void (*pin_leave)(uint64_t);
static uint64_t module;
static unsigned calls;
static pthread_barrier_t ready, start;

static uint64_t now(clockid_t clock) {
  struct timespec t;
  assert(!clock_gettime(clock, &t));
  return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

static void barrier(pthread_barrier_t *b) {
  int rc = pthread_barrier_wait(b);
  assert(!rc || rc == PTHREAD_BARRIER_SERIAL_THREAD);
}

static void borrow(void) {
  assert(enter(module));
  if (scope_leave) {
    assert(scope_leave(module));
  } else {
    pin_leave(module);
  }
}

struct row {
  uint64_t cpu;
  long voluntary, involuntary;
  int first_cpu, last_cpu;
};

static void *worker(void *opaque) {
  struct row *r = opaque;
  for (unsigned i = 0; i < 2000; i++) {
    borrow();
  }
  barrier(&ready);
  barrier(&start);
  struct rusage before, after;
  assert(!getrusage(RUSAGE_THREAD, &before));
  r->first_cpu = sched_getcpu();
  uint64_t cpu = now(CLOCK_THREAD_CPUTIME_ID);
  for (unsigned i = 0; i < calls; i++) {
    borrow();
  }
  r->cpu = now(CLOCK_THREAD_CPUTIME_ID) - cpu;
  r->last_cpu = sched_getcpu();
  assert(!getrusage(RUSAGE_THREAD, &after));
  r->voluntary = after.ru_nvcsw - before.ru_nvcsw;
  r->involuntary = after.ru_nivcsw - before.ru_nivcsw;
  return NULL;
}

static int load(void *ctx, uint64_t id, const char *image, int32_t *metadata,
                struct gw_loader_error *error) {
  (void)ctx;
  (void)id;
  (void)image;
  (void)error;
  *metadata = 1;
  return GW_OK;
}

static int close_image(void *ctx, uint64_t id, struct gw_loader_error *error) {
  (void)ctx;
  (void)id;
  (void)error;
  return GW_OK;
}

static int find(void *ctx, uint64_t id, const char *symbol, const char *sig,
                void **entry, struct gw_loader_error *error) {
  (void)ctx;
  (void)id;
  (void)symbol;
  (void)sig;
  (void)entry;
  (void)error;
  return GW_NOT_FOUND;
}

static uint64_t current(void *ctx) {
  (void)ctx;
  return 0;
}

static int resolve(void *ctx, const char *path, char image[GW_LOADER_PATH],
                   struct gw_loader_error *error, void *unused) {
  (void)ctx;
  (void)error;
  (void)unused;
  assert(strlen(path) < GW_LOADER_PATH);
  strcpy(image, path);
  return GW_OK;
}

int main(int argc, char **argv) {
  assert(argc == 5);
  int threads = atoi(argv[3]);
  calls = (unsigned)atoi(argv[4]);
  assert(threads > 0 && threads <= 32 && calls > 0);
  assert(!strcmp(argv[2], "pin") || !strcmp(argv[2], "scope"));
  void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!library) {
    fprintf(stderr, "dlopen: %s\n", dlerror());
    return 1;
  }
  const struct gw_dlopen_api_v1 *(*query)(uint32_t, size_t) =
      dlsym(library, "gw_dlopen_query");
  int (*configure)(const struct gw_dlopen_config *, struct gw_loader_error *) =
      dlsym(library, "gw_dlopen_configure");
  int (*activate)(struct gw_loader_error *) =
      dlsym(library, "gw_dlopen_activate");
  int (*stop)(struct gw_loader_error *) = dlsym(library, "gw_dlopen_stop");
  assert(query && configure && activate && stop);
  int scoped = !strcmp(argv[2], "scope");
  enter = dlsym(library, scoped ? "gw_dlopen_scope_enter" : "gw_dlopen_enter");
  scope_leave = scoped ? dlsym(library, "gw_dlopen_scope_leave") : NULL;
  pin_leave = scoped ? NULL : dlsym(library, "gw_dlopen_leave");
  assert(enter && (scope_leave || pin_leave));
  const struct gw_dlopen_api_v1 *api = query(GW_DLOPEN_ABI_V1, sizeof *api);
  struct gw_dlopen_config config = {
      .transport = {load, close_image, find, current},
      .resolve_image = resolve};
  struct gw_loader_error error;
  struct gw_dlopen_library opened;
  assert(api && configure(&config, &error) == GW_OK &&
         activate(&error) == GW_OK);
  assert(api->open(NULL, argv[1], GW_OPEN_GUEST, &opened, &error) == GW_OK);
  module = opened.handle;

  pthread_t workers[32];
  struct row rows[32] = {0};
  assert(!pthread_barrier_init(&ready, NULL, threads + 1));
  assert(!pthread_barrier_init(&start, NULL, threads + 1));
  for (int i = 0; i < threads; i++) {
    assert(!pthread_create(&workers[i], NULL, worker, &rows[i]));
  }
  barrier(&ready);
  uint64_t wall = now(CLOCK_MONOTONIC), cpu = 0;
  long voluntary = 0, involuntary = 0;
  barrier(&start);
  for (int i = 0; i < threads; i++) {
    assert(!pthread_join(workers[i], NULL));
    cpu += rows[i].cpu;
    voluntary += rows[i].voluntary;
    involuntary += rows[i].involuntary;
  }
  wall = now(CLOCK_MONOTONIC) - wall;
  assert(!pthread_barrier_destroy(&ready));
  assert(!pthread_barrier_destroy(&start));
  assert(api->close(NULL, module, &error) == GW_OK && stop(&error) == GW_OK);
  assert(!dlclose(library));
  printf(
      "SCOPE_COST {\"result\":\"PASS\",\"mode\":\"%s\",\"threads\":%d,"
      "\"calls\":%u,\"cpu_ns\":%llu,\"wall_ns\":%llu,"
      "\"voluntary\":%ld,\"involuntary\":%ld,\"workers\":[",
      argv[2], threads, threads * calls, (unsigned long long)cpu,
      (unsigned long long)wall, voluntary, involuntary);
  for (int i = 0; i < threads; i++) {
    printf("%s{\"cpu_ns\":%llu,\"first_cpu\":%d,\"last_cpu\":%d}", i ? "," : "",
           (unsigned long long)rows[i].cpu, rows[i].first_cpu,
           rows[i].last_cpu);
  }
  puts("]}");
}
