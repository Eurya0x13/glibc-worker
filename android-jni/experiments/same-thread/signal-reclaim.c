#define _GNU_SOURCE
#include "signal_profile.h"
#include "domain_runtime.h"
#include <dlfcn.h>
#include <errno.h>
#include <linux/futex.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#ifdef __ANDROID__
enum { ART_RECLAIM_SIGNAL = 49 };

static _Atomic int entered, released, finished, next_delivery;

static void publish(_Atomic int *word) {
  atomic_store_explicit(word, 1, memory_order_release);
  syscall(SYS_futex, word, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
}

static void await(_Atomic int *word) {
  while (!atomic_load_explicit(word, memory_order_acquire)) {
    syscall(SYS_futex, word, FUTEX_WAIT_PRIVATE, 0, NULL, NULL, 0);
  }
}

static void old_action(int number, siginfo_t *info, void *frame) {
  (void)frame;
  REQUIRE(number == ART_RECLAIM_SIGNAL && info->si_code == SI_TKILL);
  REQUIRE(syscall(SYS_gettid) == getpid());
  sigset_t mask;
  REQUIRE(!pthread_sigmask(SIG_SETMASK, NULL, &mask));
  REQUIRE(sigismember(&mask, SIGTERM));
  REQUIRE(!sigismember(&mask, ART_RECLAIM_SIGNAL)); /* SA_NODEFER */
  publish(&entered);
  await(&released);
  publish(&finished);
}

static void new_action(int number) {
  REQUIRE(number == ART_RECLAIM_SIGNAL && syscall(SYS_gettid) == getpid());
  publish(&next_delivery);
}

void domain_check_art_reclaim(void) {
  typedef int (*action_fn)(int, const struct sigaction *, struct sigaction *);
  typedef int (*action64_fn)(int, const struct sigaction64 *,
                             struct sigaction64 *);
  action_fn art_action = dlsym(RTLD_DEFAULT, "sigaction");
  action64_fn art_action64 = dlsym(RTLD_DEFAULT, "sigaction64");
  Dl_info art, art64;
  REQUIRE(art_action && art_action64 && dladdr((void *)art_action, &art) &&
          dladdr((void *)art_action64, &art64));
  REQUIRE(art.dli_fbase == art64.dli_fbase &&
          strstr(art.dli_fname, "libsigchain.so"));
  void *libc = dlopen("libc.so", RTLD_NOW | RTLD_LOCAL);
  action_fn kernel = libc ? dlsym(libc, "sigaction") : NULL;
  REQUIRE(kernel && getpid() != syscall(SYS_gettid));

  struct sigaction original, bound, logical;
  struct sigaction action = {.sa_sigaction = old_action,
                             .sa_flags = SA_SIGINFO | SA_NODEFER | SA_RESTART};
  sigemptyset(&action.sa_mask);
  sigaddset(&action.sa_mask, SIGTERM);
  REQUIRE(!art_action(ART_RECLAIM_SIGNAL, &action, &original));
  REQUIRE(!kernel(ART_RECLAIM_SIGNAL, NULL, &bound));
  REQUIRE(bound.sa_sigaction != old_action);
  REQUIRE(!art_action(ART_RECLAIM_SIGNAL, NULL, &logical));
  REQUIRE(logical.sa_sigaction == old_action &&
          (logical.sa_flags & SA_NODEFER) &&
          sigismember(&logical.sa_mask, SIGTERM));
  REQUIRE(!syscall(SYS_tgkill, getpid(), getpid(), ART_RECLAIM_SIGNAL));
  await(&entered);

  struct sigaction64 replacement = {.sa_handler = new_action};
  sigemptyset64(&replacement.sa_mask);
  sigaddset64(&replacement.sa_mask, SIGALRM);
  REQUIRE(!art_action64(ART_RECLAIM_SIGNAL, &replacement, NULL));
  REQUIRE(!art_action(ART_RECLAIM_SIGNAL, NULL, &logical));
  REQUIRE(logical.sa_handler == new_action &&
          !(logical.sa_flags & SA_RESTART) &&
          sigismember(&logical.sa_mask, SIGALRM));

  unsigned count = 0;
  int error = domain_late_signal_reclaim(1000, &count);
  if (error) {
    fprintf(stderr, "ART application reclaim status=%d\n", error);
  }
  REQUIRE(!error && count);
  unsigned char resident;
  REQUIRE(mincore((void *)bound.sa_sigaction, 1, &resident) == -1 &&
          errno == ENOMEM);
  REQUIRE(!atomic_load_explicit(&finished, memory_order_acquire));

  publish(&released);
  await(&finished);
  REQUIRE(!syscall(SYS_tgkill, getpid(), getpid(), ART_RECLAIM_SIGNAL));
  await(&next_delivery);
  REQUIRE(!art_action(ART_RECLAIM_SIGNAL, &original, NULL));
  printf(
      "ART_SIGNAL_RECLAIM {\"result\":\"PASS\",\"reclaimed\":%u,"
      "\"main_tid\":%d,\"ordinary_mask\":true,"
      "\"old_callback_returned\":true,\"replacement_delivered\":true}\n",
      count, getpid());
}
#endif
