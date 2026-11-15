#define _GNU_SOURCE
#include "signal-router.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t hits, depth, deepest, masked, on_alternate;
static int recurse;
static uintptr_t alternate_low, alternate_high;
static int pipe_fd[2];
static pthread_t target;
static int failures;
static _Atomic int racing, race_bad, race_hits;
static _Atomic int pause_entry, entry_waiting;
#define REQUIRE(condition)                                                    \
  do {                                                                        \
    if (!(condition)) {                                                       \
      fprintf(stderr, "signal contract failure line=%d errno=%d\n", __LINE__, \
              errno);                                                         \
      ++failures;                                                             \
    }                                                                         \
  } while (0)

void domain_signal_entry(int number, siginfo_t *info, void *context,
                         void *token) {
  if (atomic_load(&pause_entry)) {
    atomic_store(&entry_waiting, 1);
    while (atomic_load(&pause_entry)) {
    }
  }
  signal_router_forward(number, info, context, token);
}

static void handler(int number) {
  (void)number;
  int local;
  ++hits;
  ++depth;
  if (depth > deepest) {
    deepest = depth;
  }
  sigset_t mask;
  sigprocmask(SIG_SETMASK, NULL, &mask);
  masked = sigismember(&mask, 53);
  on_alternate =
      (uintptr_t)&local >= alternate_low && (uintptr_t)&local < alternate_high;
  if (recurse && hits == 1) {
    raise(52);
  }
  --depth;
  errno = EIO;
}

static void install(int flags) {
  struct sigaction action = {.sa_handler = handler, .sa_flags = flags};
  sigemptyset(&action.sa_mask);
  sigaddset(&action.sa_mask, 53);
  REQUIRE(sigaction(52, &action, NULL) == 0);
  struct sigaction queried;
  REQUIRE(sigaction(52, NULL, &queried) == 0);
  REQUIRE(queried.sa_handler == handler && queried.sa_flags == flags &&
          sigismember(&queried.sa_mask, 53));
}

static void *interrupt_read(void *unused) {
  (void)unused;
  usleep(20000);
  pthread_kill(target, 52);
  usleep(20000);
  char byte = 'x';
  if (write(pipe_fd[1], &byte, 1) != 1) {
    _exit(107);
  }
  return NULL;
}

static void restart_test(int flags) {
  install(flags);
  REQUIRE(pipe(pipe_fd) == 0);
  target = pthread_self();
  pthread_t sender;
  REQUIRE(pthread_create(&sender, NULL, interrupt_read, NULL) == 0);
  char byte;
  int result = read(pipe_fd[0], &byte, 1);
  int error = errno;
  REQUIRE(pthread_join(sender, NULL) == 0);
  if (flags & SA_RESTART) {
    REQUIRE(result == 1 && byte == 'x');
  } else {
    REQUIRE(result == -1 && error == EINTR);
    REQUIRE(read(pipe_fd[0], &byte, 1) == 1 && byte == 'x');
  }
  close(pipe_fd[0]);
  close(pipe_fd[1]);
}

static void race_a(int number) {
  (void)number;
  sigset_t mask;
  sigprocmask(SIG_SETMASK, NULL, &mask);
  atomic_fetch_add(&race_bad, sigismember(&mask, 53) != 1);
  atomic_fetch_add(&race_hits, 1);
}

static void race_b(int number) {
  (void)number;
  sigset_t mask;
  sigprocmask(SIG_SETMASK, NULL, &mask);
  atomic_fetch_add(&race_bad, sigismember(&mask, 53) != 0);
  atomic_fetch_add(&race_hits, 1);
}

static void *race_deliver(void *unused) {
  (void)unused;
  while (atomic_load(&racing)) {
    raise(52);
  }
  return NULL;
}

static void replacement_test(void) {
  struct sigaction a = {.sa_handler = race_a};
  struct sigaction b = {.sa_handler = race_b};
  sigaddset(&a.sa_mask, 53);
  REQUIRE(sigaction(52, &a, NULL) == 0);
  atomic_store(&racing, 1);
  pthread_t thread;
  REQUIRE(!pthread_create(&thread, NULL, race_deliver, NULL));
  for (int i = 0; i < 1000; ++i) {
    REQUIRE(sigaction(52, i & 1 ? &a : &b, NULL) == 0);
  }
  atomic_store(&racing, 0);
  REQUIRE(!pthread_join(thread, NULL));
  REQUIRE(atomic_load(&race_hits) > 0 && !atomic_load(&race_bad));
}

/* Pause after kernel one-shot reset, before the router can publish fired.
 * Queries and replacement must observe the reset even in this window. */
static void *one_shot_deliver(void *unused) {
  (void)unused;
  raise(52);
  return NULL;
}

static void reset_publication_test(void) {
  install(SA_RESETHAND);
  atomic_store(&pause_entry, 1);
  pthread_t thread;
  REQUIRE(!pthread_create(&thread, NULL, one_shot_deliver, NULL));
  while (!atomic_load(&entry_waiting)) {
  }
  struct sigaction previous;
  REQUIRE(!sigaction(52, NULL, &previous) && previous.sa_handler == SIG_DFL);
  struct sigaction replacement = {.sa_handler = race_a};
  sigaddset(&replacement.sa_mask, 53);
  REQUIRE(!sigaction(52, &replacement, &previous) &&
          previous.sa_handler == SIG_DFL);
  atomic_store(&pause_entry, 0);
  REQUIRE(!pthread_join(thread, NULL));
  REQUIRE(!sigaction(52, NULL, &previous) && previous.sa_handler == race_a);
}

int main(void) {
  REQUIRE(signal_router_init() == 0);
  void *memory = mmap(NULL, 65536, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  REQUIRE(memory != MAP_FAILED);
  stack_t alternate = {.ss_sp = memory, .ss_size = 65536}, original;
  REQUIRE(sigaltstack(&alternate, &original) == 0);
  alternate_low = (uintptr_t)memory;
  alternate_high = alternate_low + 65536;
  install(SA_ONSTACK);
  errno = EDOM;
  REQUIRE(raise(52) == 0);
  REQUIRE(hits == 1 && masked && on_alternate && errno == EDOM);

  hits = deepest = 0;
  recurse = 1;
  install(SA_NODEFER);
  REQUIRE(raise(52) == 0);
  REQUIRE(hits == 2 && deepest == 2 && !on_alternate);
  hits = deepest = 0;
  install(0);
  REQUIRE(raise(52) == 0);
  REQUIRE(hits == 2 && deepest == 1);
  recurse = 0;

  install(SA_RESETHAND);
  REQUIRE(raise(52) == 0);
  struct sigaction queried;
  REQUIRE(sigaction(52, NULL, &queried) == 0 && queried.sa_handler == SIG_DFL);
  pid_t child = fork();
  REQUIRE(child >= 0);
  if (child == 0) {
    raise(52);
    _exit(108);
  }
  int status = 0;
  REQUIRE(waitpid(child, &status, 0) == child);
  REQUIRE(WIFSIGNALED(status) && WTERMSIG(status) == 52);

  struct sigaction ignore = {.sa_handler = SIG_IGN};
  REQUIRE(sigaction(52, &ignore, NULL) == 0);
  sig_atomic_t before = hits;
  REQUIRE(raise(52) == 0 && hits == before);
  REQUIRE(signal(52, handler) == SIG_IGN);
  REQUIRE(raise(52) == 0 && hits == before + 1);
  REQUIRE(sigaction(52, NULL, &queried) == 0 && queried.sa_handler == handler &&
          (queried.sa_flags & SA_RESTART));
#ifdef __ANDROID__
  struct sigaction64 action64 = {.sa_handler = handler, .sa_flags = SA_ONSTACK};
  struct sigaction64 previous64;
  REQUIRE(sigaction64(52, &action64, &previous64) == 0 &&
          previous64.sa_handler == handler);
  REQUIRE(raise(52) == 0 && on_alternate);
  REQUIRE(sigaction64(52, NULL, &previous64) == 0 &&
          previous64.sa_handler == handler &&
          (previous64.sa_flags & SA_ONSTACK));
#endif
  restart_test(0);
  restart_test(SA_RESTART);
  replacement_test();
  reset_publication_test();
  REQUIRE(signal_router_verify() == 0);
  REQUIRE(signal_router_restore() == 0);
  REQUIRE(sigaltstack(&original, NULL) == 0);
  REQUIRE(munmap(memory, 65536) == 0);
  printf(
      "{\"result\":\"%s\",\"failures\":%d,\"checks\":[\"query\",\"mask\","
      "\"errno\",\"onstack\",\"nodefer\",\"reset_default\",\"ignore\","
      "\"restart\",\"eintr\",\"concurrent_replacement\",\"reset_publication\","
      "\"restore\"]}\n",
      failures ? "FAIL" : "PASS", failures);
  return !!failures;
}
