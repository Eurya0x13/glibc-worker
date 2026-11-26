#define _GNU_SOURCE
#include "nonlocal_probe.h"
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* This translation unit uses -fexceptions. Both cleanup attributes must run
 * through real DWARF forced unwind, including the interrupted thread body;
 * NPTL's C-only longjmp cleanup fallback cannot substitute for that proof. */
struct unwind_state {
  void *fault_page, *alternate;
  size_t page;
  int descriptors[2], exiting;
  _Atomic int entered, cleaned;
};

struct cleanup_record {
  struct unwind_state *state;
  int bit;
};

static _Thread_local struct unwind_state *current;
static _Thread_local int canary = 0x7193;

static void mark_cleanup(struct cleanup_record *record) {
  if (current != record->state || canary != 0x7193) {
    _exit(166);
  }
  atomic_fetch_or_explicit(&record->state->cleaned, record->bit,
                           memory_order_release);
}

static void handler(int number, siginfo_t *information, void *context) {
  struct unwind_state *state = current;
  if (!state || canary != 0x7193 || number != SIGSEGV ||
      information->si_code <= 0 || information->si_addr != state->fault_page ||
      !context) {
    _exit(167);
  }
  char local;
  uintptr_t here = (uintptr_t)&local;
  if (state->alternate && (here < (uintptr_t)state->alternate ||
                           here >= (uintptr_t)state->alternate + 65536)) {
    _exit(168);
  }

  struct cleanup_record record
      __attribute__((cleanup(mark_cleanup))) = {state, 2};
  atomic_store_explicit(&state->entered, 1, memory_order_release);
  if (state->exiting) {
    pthread_exit((void *)(uintptr_t)73);
  }
  char byte;
  if (read(state->descriptors[0], &byte, 1) < 0) {
    _exit(179);
  }
  _exit(169);
}

static void *fault_thread(void *argument) {
  struct unwind_state *state = argument;
  current = state;
  if (state->alternate) {
    stack_t alternate = {.ss_sp = state->alternate, .ss_size = 65536};
    if (sigaltstack(&alternate, NULL)) {
      _exit(170);
    }
  }
  sigset_t faults;
  sigemptyset(&faults);
  sigaddset(&faults, SIGSEGV);
  if (pthread_sigmask(SIG_UNBLOCK, &faults, NULL)) {
    _exit(171);
  }
  struct cleanup_record record
      __attribute__((cleanup(mark_cleanup))) = {state, 1};
  int value = *(volatile int *)state->fault_page;
  return (void *)(intptr_t)value;
}

static int await_handler(struct unwind_state *state) {
  struct timespec start, now;
  if (clock_gettime(CLOCK_MONOTONIC, &start)) {
    return -1;
  }
  while (!atomic_load_explicit(&state->entered, memory_order_acquire)) {
    if (clock_gettime(CLOCK_MONOTONIC, &now) || now.tv_sec - start.tv_sec > 3) {
      return -1;
    }
    struct timespec pause = {.tv_nsec = 1000000};
    nanosleep(&pause, NULL);
  }
  return 0;
}

int sdk_guest_signal_unwind_probe(int mode) {
  struct unwind_state state = {.page = (size_t)sysconf(_SC_PAGESIZE),
                               .exiting = mode >= 99};
  state.fault_page =
      mmap(NULL, state.page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (state.fault_page == MAP_FAILED || pipe(state.descriptors)) {
    _exit(172);
  }
  if (!(mode % 2)) {
    state.alternate = mmap(NULL, 65536, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (state.alternate == MAP_FAILED) {
      _exit(173);
    }
  }

  struct sigaction previous,
      action = {.sa_sigaction = handler,
                .sa_flags = SA_SIGINFO | (state.alternate ? SA_ONSTACK : 0)};
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGSEGV, &action, &previous)) {
    _exit(174);
  }
  pthread_t thread;
  if (pthread_create(&thread, NULL, fault_thread, &state) ||
      await_handler(&state)) {
    _exit(175);
  }
  if (!state.exiting && pthread_cancel(thread)) {
    _exit(176);
  }
  void *result;
  if (pthread_join(thread, &result)) {
    _exit(177);
  }
  int valid =
      result == (state.exiting ? (void *)(uintptr_t)73 : PTHREAD_CANCELED) &&
      atomic_load_explicit(&state.cleaned, memory_order_acquire) == 3;
  if (!valid) {
    fprintf(stderr, "SIGNAL_UNWIND mode=%d cleaned=%d result=%p\n", mode,
            atomic_load_explicit(&state.cleaned, memory_order_acquire), result);
  }

  if (sigaction(SIGSEGV, &previous, NULL) || close(state.descriptors[0]) ||
      close(state.descriptors[1]) || munmap(state.fault_page, state.page) ||
      (state.alternate && munmap(state.alternate, 65536))) {
    _exit(178);
  }
  return valid ? 73 : -97;
}

#ifdef GW_UNWIND_NATIVE_CONTROL
int main(void) {
  for (int mode = 97; mode <= 100; ++mode) {
    int value = sdk_guest_signal_unwind_probe(mode);
    printf("{\"mode\":%d,\"value\":%d}\n", mode, value);
    if (value != 73) {
      return 1;
    }
  }
  return 0;
}
#endif
