#define _GNU_SOURCE
#include "nonlocal.h"
#include <assert.h>
#include <dlfcn.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

static sigjmp_buf target;
static sigjmp_buf unobserved_target;
static volatile sig_atomic_t cleaned, handled;
static struct gw_nonlocal_scope *route_target;
static volatile sig_atomic_t routed_cleaned;
_Noreturn void fortified_jump(jmp_buf, int);

static void escape(int number) {
  assert(number == SIGUSR1);
  handled++;
  siglongjmp(target, 19);
}

static void nested(unsigned depth, int signal_jump) {
  struct gw_nonlocal_scope scope;
  if (!sigsetjmp(scope.landing, 0)) {
    gw_nonlocal_enter(&scope);
    if (depth) {
      nested(depth - 1, signal_jump);
    } else if (signal_jump == 1) {
      assert(!raise(SIGUSR1));
    } else if (!signal_jump) {
      longjmp(target, 19);
    } else if (signal_jump == 3) {
      fortified_jump(target, 19);
    }
  }
  ++cleaned;
  gw_nonlocal_leave(&scope);
}

static void surviving_outer(void) {
  struct gw_nonlocal_scope outer;
  assert(!sigsetjmp(outer.landing, 0));
  gw_nonlocal_enter(&outer);
  int value = sigsetjmp(target, 1);
  if (!value) {
    nested(4, 0);
  }
  assert(value == 19 && !gw_nonlocal_pending());
  gw_nonlocal_leave(&outer);
}

static void unobserved_capture(void) {
  pid_t child = fork();
  assert(child >= 0);
  if (!child) {
#ifdef __ANDROID__
    int (*capture)(sigjmp_buf, int) = dlsym(RTLD_NEXT, "sigsetjmp");
#else
    int (*capture)(sigjmp_buf, int) = dlsym(RTLD_NEXT, "__sigsetjmp");
#endif
    if (!capture) {
      _exit(41);
    }
    if (!capture(unobserved_target, 0)) {
      struct gw_nonlocal_scope scope;
      if (!sigsetjmp(scope.landing, 0)) {
        gw_nonlocal_enter(&scope);
        siglongjmp(unobserved_target, 1);
      }
      gw_nonlocal_leave(&scope);
    }
    _exit(42);
  }
  int status;
  assert(waitpid(child, &status, 0) == child);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 125);
}

static void route_nested(int depth) {
  struct gw_nonlocal_scope scope;
  if (!sigsetjmp(scope.landing, 0)) {
    gw_nonlocal_enter(&scope);
    if (depth) {
      route_nested(depth - 1);
    } else {
      gw_nonlocal_route(route_target);
    }
    assert(!"Directed transfer returned to its source");
  }
  ++routed_cleaned;
  gw_nonlocal_leave(&scope);
}

static void directed_transfer(void) {
  sigset_t before, after;
  assert(!sigprocmask(SIG_SETMASK, NULL, &before));
  struct gw_nonlocal_scope scope;
  route_target = &scope;
  if (!sigsetjmp(scope.landing, 0)) {
    gw_nonlocal_enter(&scope);
    route_nested(39);
  }
  assert(routed_cleaned == 40 && !gw_nonlocal_pending());
  assert(!sigprocmask(SIG_SETMASK, NULL, &after));
  assert(sigismember(&after, SIGUSR1));
  gw_nonlocal_leave(&scope);
  assert(!sigprocmask(SIG_SETMASK, &before, NULL));
  /* Reuse the stack and target address under a fresh scope generation. */
  routed_cleaned = 0;
}

int main(void) {
  assert(!gw_nonlocal_init());
  struct sigaction action = {.sa_handler = escape};
  sigemptyset(&action.sa_mask);
  assert(!sigaction(SIGUSR1, &action, NULL));
  nested(2, 2);
  for (unsigned round = 0; round < 20; ++round) {
    int value = sigsetjmp(target, 1);
    if (!value) {
      nested(23, 1);
    }
    assert(value == 19 && !gw_nonlocal_pending());
    sigset_t mask;
    assert(!sigprocmask(SIG_SETMASK, NULL, &mask));
    assert(!sigismember(&mask, SIGUSR1));
    surviving_outer();
  }
  assert(cleaned == 583 && handled == 20);
  if (!sigsetjmp(target, 1)) {
    nested(11, 3);
  }
  assert(cleaned == 595 && !gw_nonlocal_pending());
  unobserved_capture();
  for (int i = 0; i < 20; ++i) {
    directed_transfer();
  }
  puts(
      "{\"result\":\"PASS\",\"boundaries\":595,\"signals\":20,"
      "\"fortified_call_tested\":true,\"unobserved_rejected\":true,"
      "\"directed_scopes\":800,"
#ifdef __GLIBC__
      "\"glibc_checked_entry\":true}");
#else
      "\"glibc_checked_entry\":false}");
#endif
}
