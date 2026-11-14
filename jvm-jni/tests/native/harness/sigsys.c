#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/syscall.h>
#include "raw_syscall.h"
static int report_fd = -1;

/* Diagnostic only: no emulation of denied calls. Works even with FS cleared. */
static __attribute__((always_inline)) inline void append(char **p,
                                                         const char *s) {
  while (*s) {
    *(*p)++ = *s++;
  }
}

static __attribute__((always_inline)) inline void number(char **p, uintptr_t n,
                                                         unsigned base) {
  char tmp[32];
  unsigned i = 0;
  do {
    tmp[i++] = "0123456789abcdef"[n % base];
    n /= base;
  } while (n);
  while (i) {
    *(*p)++ = tmp[--i];
  }
}

static __attribute__((optnone, no_stack_protector)) void handler(
    int signal, siginfo_t *info, void *context) {
  (void)signal;
  (void)context;
  char text[256], *p = text;

  append(&p, "BLOCKED SIGSYS syscall=");
  number(&p, (uintptr_t)info->si_syscall, 10);
  append(&p, " arch=0x");
  number(&p, info->si_arch, 16);
  append(&p, " call_addr=0x");
  number(&p, (uintptr_t)info->si_call_addr, 16);
  append(&p, "\nAPP_RESULT=BLOCKED\n");

  gw_raw3(SYS_write, report_fd, (long)text, p - text);
  gw_raw3(SYS_exit_group, 159, 0, 0); /* Disposable :probe process only. */
  __builtin_unreachable();
}

int install_sigsys_diagnostic(int fd) {
  report_fd = fd;
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = handler;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  return sigaction(SIGSYS, &sa, NULL);
}
