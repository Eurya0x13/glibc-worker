#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

static volatile sig_atomic_t received;

static void handler(int number) {
  assert(number == SIGUSR1);
  ++received;
  errno = EILSEQ;
}

int main(void) {
  struct sigaction action = {.sa_handler = handler};
  assert(!sigaction(SIGUSR1, &action, NULL));
  for (int i = 0; i < 6; ++i) {
    errno = EBADF;
    int result;
    switch (i) {
      case 0:
        result = pthread_kill(pthread_self(), SIGUSR1);
        break;
      case 1:
        result = pthread_sigqueue(pthread_self(), SIGUSR1, (union sigval){0});
        break;
      case 2:
        result = raise(SIGUSR1);
        break;
      case 3:
        result = gsignal(SIGUSR1);
        break;
      case 4:
        result = kill(getpid(), SIGUSR1);
        break;
      default:
        result = sigqueue(getpid(), SIGUSR1, (union sigval){0});
        break;
    }
    assert(!result && received == i + 1 && errno == EILSEQ);
  }
  puts("{\"result\":\"PASS\",\"native_handler_errno\":true,\"cases\":6}");
}
