/* Reproduce late-DSO loading without putting an existing game process at risk.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#ifdef ROUTER_STUB
void domain_signal_entry(int number, siginfo_t *info, void *context) {
  (void)number;
  (void)info;
  (void)context;
  _exit(
      111); /* This probe inspects registrations; it never delivers a signal. */
}
#else
static void existing_handler(int number) {
  (void)number;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    return 1;
  }
  struct sigaction action = {.sa_handler = existing_handler};
  if (sigaction(SIGUSR2, &action, NULL)) {
    return 2;
  }
  void *library = dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL);
  if (!library) {
    fprintf(stderr, "%s\n", dlerror());
    return 3;
  }
  int (*init)(void) = dlsym(library, "signal_router_init");
  int (*restore)(void) = dlsym(library, "signal_router_restore");
  if (!init || !restore) {
    return 4;
  }
  int status = init();
  struct sigaction actual;
  if (sigaction(SIGUSR2, NULL, &actual)) {
    return 5;
  }
  int wrapped = actual.sa_handler != existing_handler;
  int interposed =
      dlsym(RTLD_DEFAULT, "sigaction") == dlsym(library, "sigaction");
  printf(
      "{\"result\":\"%s\",\"init_status\":%d,\"existing_handler_wrapped\":%s,"
      "\"global_sigaction_interposed\":%s}\n",
      status || !wrapped || !interposed ? "UNSUPPORTED" : "READY", status,
      wrapped ? "true" : "false", interposed ? "true" : "false");
  if (!status && restore()) {
    return 6;
  }
  return 0;
}
#endif
