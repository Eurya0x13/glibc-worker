#define _GNU_SOURCE
#include "signal-stdio.h"
#include "signal-router.h"
#include "signals.h"
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/signalfd.h>
#include <wchar.h>
#ifdef __GLIBC__
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#endif

/* The same behavioral test can be linked to the preceding SDK to reproduce
 * the GNU hidden-read bug or confirm Bionic's existing native path. Older
 * SDKs lack the optional profile query; skipping it must not skip metadata
 * or native-FILE assertions. */
extern const char *signal_stdio_profile(void) __attribute__((weak));

static _Thread_local struct signal_slot *recipient;
static int (*native_signalfd)(int, const sigset_t *, int);

static void queue_packet(int virtual) {
  siginfo_t information = {.si_signo = SIGTRAP, .si_code = SI_QUEUE};
  information.si_pid = getpid();
  information.si_uid = getuid();
  information.si_value.sival_int = 7193;

  if (virtual) {
    assert(!signal_host_defer(recipient, &information));
  } else {
    assert(!syscall(SYS_rt_tgsigqueueinfo, getpid(), syscall(SYS_gettid),
                    SIGTRAP, &information));
  }
}

static FILE *open_stream(int virtual, const char *mode) {
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGTRAP);

  int descriptor = (virtual ? signalfd : native_signalfd)(
      -1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
  assert(descriptor >= 0);

  FILE *stream = fdopen(descriptor, mode);
  assert(stream && fileno(stream) == descriptor);

  return stream;
}

static void check_metadata(const struct signalfd_siginfo *record) {
  if (record->ssi_signo != SIGTRAP) {
    fprintf(stderr, "Expected signal %d, stdio read signal %u\n", SIGTRAP,
            record->ssi_signo);
  }
  assert(record->ssi_signo == SIGTRAP && record->ssi_code == SI_QUEUE);
  assert(record->ssi_pid == (unsigned)getpid() &&
         record->ssi_uid == (unsigned)getuid() && record->ssi_int == 7193);
}

static void buffered_reads(int virtual, struct signalfd_siginfo *record) {
  FILE *stream = open_stream(virtual, "rm");
  assert(!fwide(stream, 0));
  queue_packet(virtual);

  unsigned char *bytes = (void *)record;
  assert(fread(bytes, 1, 17, stream) == 17);
  assert(fread_unlocked(bytes + 17, 1, sizeof *record - 17, stream) ==
         sizeof *record - 17);
  check_metadata(record);
  assert(!ferror(stream) && !feof(stream));
  assert(fwide(stream, 0) < 0);

  errno = 0;
  assert(fread(record, sizeof *record, 1, stream) == 0);
  assert(ferror(stream) && errno == EAGAIN);

  clearerr(stream);
  queue_packet(virtual);
  assert(fread(record, sizeof *record, 1, stream) == 1);
  check_metadata(record);

  assert(!fclose(stream));
}

static void unbuffered_read(int virtual) {
  FILE *stream = open_stream(virtual, "rb");
  assert(!setvbuf(stream, NULL, _IONBF, 0));
  queue_packet(virtual);

  struct signalfd_siginfo record;
  errno = 0;
  assert(fgetc(stream) == EOF);
  assert(ferror(stream) && errno == EINVAL);

  clearerr(stream);
  char buffer[512];
  assert(!setvbuf(stream, buffer, _IOFBF, sizeof buffer));
  assert(fread(&record, sizeof record, 1, stream) == 1);
  check_metadata(&record);

  assert(!fclose(stream));
}

static void wide_read(int virtual) {
  FILE *stream = open_stream(virtual, "r");
  assert(fwide(stream, 1) > 0);
  queue_packet(virtual);

  assert(fgetwc(stream) == SIGTRAP);
  assert(!ferror(stream));

  assert(!fclose(stream));
}

static void reopen_stream(int virtual) {
  FILE *stream = open_stream(virtual, "r");
  int descriptor = fileno(stream);
  queue_packet(virtual);

  struct signalfd_siginfo record;
  assert(fread(&record, sizeof record, 1, stream) == 1);
  check_metadata(&record);

  FILE *reopened = freopen("/dev/null", "r", stream);
  assert(reopened == stream && fileno(stream) == descriptor);
  assert(fgetc(stream) == EOF && feof(stream));

  assert(!fclose(stream));
  assert(fcntl(descriptor, F_GETFD) == -1 && errno == EBADF);
}

static void ordinary_file(void) {
  FILE *stream = tmpfile();
  assert(stream);
  assert(fputs("native stdio\n", stream) >= 0);

  rewind(stream);
  char buffer[64];
  assert(fgets(buffer, sizeof buffer, stream) == buffer);
  assert(!strcmp(buffer, "native stdio\n"));

  assert(!fclose(stream));
}

static void *thread_reader(void *unused) {
  (void)unused;
  struct exchange exchange = {.host_tid = syscall(SYS_gettid)};
  recipient = signal_acquire(&exchange);
  assert(recipient);

  struct signalfd_siginfo information;
  buffered_reads(1, &information);

  assert(!signal_prepare_release(recipient, 0));
  signal_release(recipient);
  return NULL;
}

#ifdef __GLIBC__
static void denied_write_permission(void) {
  pid_t child = fork();
  assert(child >= 0);
  if (!child) {
    const struct sock_filter instructions[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_mprotect, 0, 3),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                 offsetof(struct seccomp_data, args[2])),
        BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, PROT_WRITE, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)};
    const struct sock_fprog filter = {
        .len = sizeof instructions / sizeof instructions[0],
        .filter = (void *)instructions};
    assert(!prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0));
    assert(!prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &filter));

    assert(!signal_router_init());
    const int notifications[] = {SIGRTMIN,     SIGRTMIN + 1, SIGRTMIN + 2,
                                 SIGRTMIN + 3, SIGRTMIN + 4, SIGRTMIN + 5};
    assert(signal_router_defer_faults(notifications) == EPERM);
    assert(!signal_stdio_profile());
    ordinary_file();

    assert(!signal_router_restore());
    _exit(0);
  }

  int status;
  assert(waitpid(child, &status, 0) == child);
  assert(WIFEXITED(status) && !WEXITSTATUS(status));
}
#endif

int main(void) {
  assert(!signal_router_init());
  const int notifications[] = {SIGRTMIN,     SIGRTMIN + 1, SIGRTMIN + 2,
                               SIGRTMIN + 3, SIGRTMIN + 4, SIGRTMIN + 5};
  assert(!signal_router_defer_faults(notifications));
  if (signal_stdio_profile) {
    assert(signal_stdio_profile());
  }

  sigset_t mask, inherited;
  sigemptyset(&mask);
  sigaddset(&mask, SIGTRAP);
  assert(!pthread_sigmask(SIG_BLOCK, &mask, &inherited));

  struct exchange exchange = {.host_tid = syscall(SYS_gettid)};
  recipient = signal_acquire(&exchange);
  assert(recipient);
  native_signalfd = (void *)dlsym(RTLD_NEXT,
#ifdef __ANDROID__
                                  "signalfd64"
#else
                                  "signalfd"
#endif
  );
  assert(native_signalfd);

  struct signalfd_siginfo native, translated;
  buffered_reads(0, &native);
  buffered_reads(1, &translated);
  assert(!memcmp(&native, &translated, sizeof native));
  for (int virtual = 0; virtual < 2; ++virtual) {
    unbuffered_read(virtual);
    wide_read(virtual);
    reopen_stream(virtual);
  }
  ordinary_file();

  pthread_t readers[16];
  for (unsigned i = 0; i < 16; ++i) {
    assert(!pthread_create(&readers[i], NULL, thread_reader, NULL));
  }
  for (unsigned i = 0; i < 16; ++i) {
    assert(!pthread_join(readers[i], NULL));
  }

  assert(!signal_prepare_release(recipient, 0));
  signal_release(recipient);
  assert(!pthread_sigmask(SIG_SETMASK, &inherited, NULL));
  assert(!signal_router_restore());
  if (signal_stdio_profile) {
    assert(!signal_stdio_profile());
  }
  ordinary_file();
#ifdef __GLIBC__
  if (signal_stdio_profile) {
    denied_write_permission();
  }
#endif

  puts(
      "{\"result\":\"PASS\",\"native_file_identity\":true,"
      "\"buffered_partial_reads\":true,\"unlocked_reads\":true,"
      "\"native_metadata_comparison\":true,\"native_unbuffered_error\":true,"
      "\"wide_orientation\":true,\"freopen_and_close\":true,"
      "\"ordinary_files\":true,\"native_profile_restore\":true,"
      "\"per_reader_recipient\":true"
#ifdef __GLIBC__
      ",\"denied_write_permission\":true"
#endif
      "}");
}
