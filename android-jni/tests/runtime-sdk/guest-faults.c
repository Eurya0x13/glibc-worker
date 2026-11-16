#define _GNU_SOURCE
#include "nonlocal_probe.h"
#include <errno.h>
#include <fenv.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

static __thread int tls_canary = 0x7193;
static __thread uintptr_t expected_thread;
static __thread void *fault_page;
static __thread size_t fault_size;
static __thread int mode;
static __thread int expected_signal;
static __thread volatile sig_atomic_t handled, resumed;
static __thread sigjmp_buf checkpoint;
static __thread ucontext_t *saved_context;
static __thread int external_registration;
static void trigger_trap(void);

static void fault_handler(int number, siginfo_t *info, void *frame) {
  if (number != expected_signal || info->si_code <= 0 ||
      ((number == SIGSEGV || number == SIGBUS) &&
       info->si_addr != fault_page) ||
      !frame || tls_canary != 0x7193 ||
      (uintptr_t)pthread_self() != expected_thread || errno != EDOM) {
    _exit(128);
  }
  ++handled;
  if (number == SIGFPE && info->si_code != FPE_FLTDIV) {
    _exit(161);
  }
  if (number == SIGSYS &&
      (info->si_code != 1 || info->si_syscall != SYS_getpid ||
#if defined(__aarch64__)
       info->si_arch != AUDIT_ARCH_AARCH64 ||
#else
       info->si_arch != AUDIT_ARCH_X86_64 ||
#endif
       !info->si_call_addr)) {
    _exit(144);
  }
  sigset_t effective;
  if (pthread_sigmask(SIG_SETMASK, NULL, &effective) ||
      sigismember(&effective, number) != (mode != 26) ||
      sigismember(&((ucontext_t *)frame)->uc_sigmask, number)) {
    _exit(142);
  }
  if (mode == 26) {
    if (handled == 1) {
      trigger_trap();
      if (handled != 2) {
        _exit(143);
      }
      siglongjmp(checkpoint, 31);
    }
#if defined(__aarch64__)
    /* BRK saves its own PC; x86 INT3 saves the following PC. This nested
     * handler deliberately returns, so advance only the known test BRK.
     * https://github.com/torvalds/linux/blob/v6.12/arch/arm64/kernel/debug-monitors.c
     */
    ((ucontext_t *)frame)->uc_mcontext.pc += 4;
#endif
    return;
  }
  if (mode == 36) {
    sigjmp_buf local;
    if (!sigsetjmp(local, 1)) {
      siglongjmp(local, 17);
    }
    siglongjmp(checkpoint, 31);
  }
  if (mode == 23 || mode == 31) {
    if (syscall(SYS_mprotect, fault_page, fault_size, PROT_READ | PROT_WRITE)) {
      _exit(129);
    }
    if (mode == 31) {
      ucontext_t *context = frame;
      sigaddset(&context->uc_sigmask, SIGUSR1);
      sigaddset(&context->uc_sigmask, SDK_HOST_ESCAPE_SIGNAL);
    }
    return;
  }
  if (mode == 24) {
    resumed = 1;
    setcontext(saved_context);
    _exit(130);
  }
  siglongjmp(checkpoint, 31);
}

static void *thread_fault(void *argument) {
  external_registration = 1;
  intptr_t mode = (intptr_t)argument;
  int result = sdk_guest_fault_probe((int)mode);
  return (void *)(intptr_t)result;
}

static int thread_faults(void) {
  struct sigaction previous,
      action = {.sa_sigaction = fault_handler, .sa_flags = SA_SIGINFO};
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGSEGV, &action, &previous)) {
    return -39;
  }
  pthread_t workers[8];
  for (unsigned i = 0; i < 8; ++i) {
    intptr_t mode = i % 2 ? 24 : 22;
    if (pthread_create(&workers[i], NULL, thread_fault, (void *)mode)) {
      _exit(135);
    }
  }
  int result = 73;
  for (unsigned i = 0; i < 8; ++i) {
    void *value;
    if (pthread_join(workers[i], &value) || (intptr_t)value != 73) {
      result = -40;
    }
  }
  if (sigaction(SIGSEGV, &previous, NULL)) {
    result = -41;
  }
  return result;
}

static void trigger_trap(void) {
#if defined(__x86_64__)
  __asm__ volatile("int3" ::: "memory");
#else
  __asm__ volatile("brk #0" ::: "memory");
#endif
}

static int install_test_seccomp(void) {
  /* Affect only this disposable test carrier. The magic ignored syscall
   * argument keeps ordinary getpid calls working after this probe returns.
   * A seccomp filter cannot be removed; run in an isolated consumer process.
   * https://docs.kernel.org/userspace-api/seccomp_filter.html
   */
  const struct sock_filter instructions[] = {
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_getpid, 0, 3),
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args)),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0x47575359, 0, 1),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRAP),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)};
  const struct sock_fprog program = {
      .len = sizeof instructions / sizeof instructions[0],
      .filter = (void *)instructions};
  return prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) ||
         prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program);
}

static void unexpected_blocked_handler(int number) {
  (void)number;
  _exit(156);
}

static _Noreturn void default_fault(int selected) {
  if (selected == 35) {
    pthread_t thread;
    if (pthread_create(&thread, NULL, thread_fault, (void *)(intptr_t)33)) {
      _exit(136);
    }
    pthread_join(thread, NULL);
    _exit(137);
  }
  struct sigaction action = {.sa_handler = selected == 82 ? SIG_IGN : SIG_DFL};
  if (selected == 83) {
    action.sa_handler = unexpected_blocked_handler;
  }
  sigemptyset(&action.sa_mask);
  if (selected == 34) {
    /* Consume the Guest one-shot, then test that the second real fault uses
     * Guest SIG_DFL rather than HotSpot's Host action or a synthetic exit. */
    mode = 28;
    expected_signal = SIGTRAP;
    expected_thread = (uintptr_t)pthread_self();
    handled = 0;
    action.sa_sigaction = fault_handler;
    action.sa_flags = SA_SIGINFO | SA_RESETHAND;
  }
  if (sigaction(SIGTRAP, &action, NULL)) {
    _exit(138);
  }
  if (selected == 83) {
    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGTRAP);
    if (pthread_sigmask(SIG_BLOCK, &blocked, NULL)) {
      _exit(157);
    }
  }
  if (selected == 34 && !sigsetjmp(checkpoint, 1)) {
    errno = EDOM;
    trigger_trap();
    _exit(139);
  }
  dprintf(2, "{\"stage\":\"guest-default-execute\",\"mode\":%d,\"tid\":%ld}\n",
          selected, syscall(SYS_gettid));
  trigger_trap();
  _exit(140);
}

int sdk_guest_fault_probe(int selected) {
  if ((selected >= 33 && selected <= 35) || selected == 82 || selected == 83) {
    default_fault(selected);
  }
  if (selected == 32) {
    return thread_faults();
  }
  fenv_t original_fenv;
  if (selected == 87) {
    if (fegetenv(&original_fenv)) {
      return -87;
    }
  }
  mode = selected;
  expected_signal = (selected == 28 || selected == 26 || selected == 84)
                        ? SIGTRAP
                    : selected == 29 ? SIGILL
                    : selected == 30 ? SIGBUS
                    : selected == 37 ? SIGSYS
                    : selected == 87 ? SIGFPE
                                     : SIGSEGV;
  handled = resumed = 0;
  expected_thread = (uintptr_t)pthread_self();
  fault_size = (size_t)sysconf(_SC_PAGESIZE);
  fault_page =
      mmap(NULL, fault_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (fault_page == MAP_FAILED) {
    return -30;
  }
  if (selected == 30) {
    /* A mapping beyond an empty regular file produces a real BUS_ADRERR on
     * both architectures, without an unportable integer divide instruction. */
    int file = (int)syscall(SYS_memfd_create, "gw-guest-bus", 1);
    if (file < 0 || munmap(fault_page, fault_size)) {
      _exit(133);
    }
    fault_page = mmap(NULL, fault_size, PROT_READ, MAP_PRIVATE, file, 0);
    close(file);
    if (fault_page == MAP_FAILED) {
      _exit(134);
    }
  }
  struct sigaction action = {.sa_sigaction = fault_handler,
                             .sa_flags = SA_SIGINFO};
  if (selected == 25) {
    action.sa_flags |= SA_RESETHAND;
  }
  if (selected == 26) {
    action.sa_flags |= SA_NODEFER;
  }
  sigemptyset(&action.sa_mask);
  struct sigaction previous;
  if (!external_registration &&
      sigaction(expected_signal, &action, &previous)) {
    munmap(fault_page, fault_size);
    return -31;
  }
  sigset_t signal, original, observed;
  sigemptyset(&signal);
  sigaddset(&signal, expected_signal);
  if (pthread_sigmask(SIG_UNBLOCK, &signal, &original)) {
    _exit(131);
  }
  ucontext_t context;
  saved_context = &context;
  if (getcontext(&context)) {
    _exit(132);
  }
  volatile int result = 73;
  /* AArch64 integer division does not trap on zero. Request a real FP
   * divide trap and distinguish native hardware support from bridge
   * dispatch; an unavailable exception enable is not a recovery pass.
   * https://github.com/bminor/glibc/blob/glibc-2.41/sysdeps/aarch64/fpu/feenablxcpt.c
   */
  if (selected == 87) {
    if (feclearexcept(FE_ALL_EXCEPT)) {
      _exit(162);
    }
    if (feenableexcept(FE_DIVBYZERO) < 0) {
      result = 74;
      goto finish;
    }
  }
  if (selected == 37 && install_test_seccomp()) {
    _exit(145);
  }
  if (!resumed) {
    int transferred = sigsetjmp(checkpoint, selected != 27);
    if (!transferred) {
      errno = EDOM;
      if (selected == 87) {
        double one = 1.0, zero = 0.0;
#if defined(__x86_64__)
        __asm__ volatile("divsd %1, %0" : "+x"(one) : "x"(zero));
#else
        __asm__ volatile("fdiv %d0, %d0, %d1" : "+w"(one) : "w"(zero));
#endif
        _exit(160); /* Enabling traps without receiving one is not success. */
      }
      if (selected == 37) {
        syscall(SYS_getpid, 0x47575359);
        _exit(146);
      }
      if (selected == 84) {
        sigset_t logical;
        uint64_t physical;
        if (pthread_sigmask(SIG_SETMASK, NULL, &logical) ||
            syscall(SYS_rt_sigprocmask, SIG_SETMASK, NULL, &physical, 8)) {
          _exit(158);
        }
        dprintf(STDOUT_FILENO,
                "{\"stage\":\"guest-fault-mask\",\"signal\":%d,"
                "\"logical_blocked\":%s,\"physical_blocked\":%s}\n",
                SIGTRAP, sigismember(&logical, SIGTRAP) ? "true" : "false",
                physical & (UINT64_C(1) << (SIGTRAP - 1)) ? "true" : "false");
        for (int i = 0; i < 2; ++i) {
          siginfo_t message = {.si_signo = SIGTRAP, .si_code = SI_QUEUE};
          message.si_pid = getpid();
          message.si_uid = getuid();
          message.si_value.sival_int = 0x7193 + i;
          if (syscall(SYS_rt_tgsigqueueinfo, getpid(), syscall(SYS_gettid),
                      SIGTRAP, &message)) {
            _exit(159);
          }
        }
        errno = EDOM;
      }
      if (selected == 28 || selected == 26 || selected == 84) {
#if defined(__x86_64__)
        __asm__ volatile("int3" ::: "memory");
#else
        __asm__ volatile("brk #0" ::: "memory");
#endif
      } else if (selected == 29) {
#if defined(__x86_64__)
        __asm__ volatile("ud2" ::: "memory");
#else
        __asm__ volatile(".inst 0" ::: "memory");
#endif
      }
      volatile unsigned char byte = *(volatile unsigned char *)fault_page;
      (void)byte;
      if (selected != 23 && selected != 31) {
        result = -32;
      }
    } else if (transferred != 31) {
      result = -33;
    }
  }
  if (handled != (selected == 26 ? 2 : 1) || tls_canary != 0x7193) {
    result = -34;
  }
  if (pthread_sigmask(SIG_SETMASK, NULL, &observed) ||
      sigismember(&observed, expected_signal) != (selected == 27)) {
    result = -35;
  }
  if (selected == 31 && (sigismember(&observed, SIGUSR1) != 1 ||
                         sigismember(&observed, SDK_HOST_ESCAPE_SIGNAL) != 1)) {
    result = -38;
  }
  if (selected == 25) {
    struct sigaction current;
    if (sigaction(SIGSEGV, NULL, &current) || current.sa_handler != SIG_DFL) {
      result = -36;
    }
  }
finish:
  if (pthread_sigmask(SIG_SETMASK, &original, NULL) ||
      (!external_registration && sigaction(expected_signal, &previous, NULL)) ||
      munmap(fault_page, fault_size)) {
    result = -37;
  }
  saved_context = NULL;
  if (selected == 87) {
    if (fesetenv(&original_fenv)) {
      return -89;
    }
    if (result == 74) {
      dprintf(STDOUT_FILENO,
              "{\"stage\":\"guest-fpe\",\"hardware_trap\":false,"
              "\"recovered\":false,\"skip\":\"ENOTSUP\"}\n");
      return 74;
    }
    dprintf(STDOUT_FILENO,
            "{\"stage\":\"guest-fpe\",\"hardware_trap\":true,"
            "\"recovered\":%s,\"skip\":null}\n",
            result == 73 ? "true" : "false");
  }
  return result;
}
