/*
 * Copyright (c) 2013, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */
/* JLI call adapted from PojavLauncher jre_launcher.c, commit
 * b12ad048157b3aa255d078c235dd4571e1900309. Test fixture only.
 * Source identity: tests/config/pojav-startup.json. */
#include "pojav_jli.h"
#include "jre-layout.h"
#include <dlfcn.h>
#include <errno.h>
#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <spawn.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef jint (*JLI_Launch_func)(int, char **, int, const char **, int,
                                const char **, const char *, const char *,
                                const char *, const char *, jboolean, jboolean,
                                jboolean, jint);

#if JRE_LAUNCHER_JNI_BASELINE
extern char **environ;

static char **baseline_environment(const char *host, char *preload,
                                   size_t size) {
  const char *slash = strrchr(host, '/');
  if (!slash || snprintf(preload, size, "LD_PRELOAD=%.*s/libjre_exec_compat.so",
                         (int)(slash - host), host) >= (int)size) {
    return NULL;
  }

  size_t count = 0;
  while (environ[count]) {
    count++;
  }

  char **copy = calloc(count + 2, sizeof *copy);
  if (copy) {
    size_t used = 0;
    for (size_t i = 0; i < count; i++) {
      if (strncmp(environ[i], "LD_PRELOAD=", 11)) {
        copy[used++] = environ[i];
      }
    }
    copy[used] = preload;
  }

  return copy;
}

static int wait_baseline(pid_t pid, int *status) {
  struct timespec now, pause = {0, 20000000};
  clock_gettime(CLOCK_MONOTONIC, &now);
  time_t deadline = now.tv_sec + 30;

  for (;;) {
    pid_t result = waitpid(pid, status, WNOHANG);
    if (result == pid) {
      return 1;
    }
    if (result < 0 && errno != EINTR) {
      break;
    }
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec >= deadline) {
      break;
    }
    nanosleep(&pause, NULL);
  }

  kill(pid, SIGKILL);
  while (waitpid(pid, status, 0) < 0 && errno == EINTR) {
  }
  return 0;
}

static int launcher_baseline(int argc, char **argv) {
  char **baseline = calloc((size_t)argc + 2, sizeof *baseline);
  char path[PATH_MAX];
  const char *home = getenv("JAVA_HOME");
  int fd = -1;

  if (home && snprintf(path, sizeof path, "%s/launcher-baseline-XXXXXX", home) <
                  (int)sizeof path) {
    fd = mkstemp(path);
    if (fd >= 0) {
      unlink(path);
      fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
  }

  FILE *log = fd >= 0 ? fdopen(fd, "w+") : NULL;
  if (!log && fd >= 0) {
    close(fd);
  }

  if (!baseline || !log) {
    free(baseline);
    if (log) {
      fclose(log);
    }
    return 1;
  }

  baseline[0] = "/system/bin/linker64";
  for (int i = 0; i < argc; i++) {
    baseline[i + 1] = argv[i];
  }
  baseline[argc - 1] = "dev.glibc.worker.JreBaseline";

  posix_spawn_file_actions_t actions;
  int initialized = posix_spawn_file_actions_init(&actions) == 0;
  int rc = initialized
               ? posix_spawn_file_actions_adddup2(&actions, fileno(log), 1)
               : 1;
  if (!rc) {
    rc = posix_spawn_file_actions_adddup2(&actions, fileno(log), 2);
  }

  pid_t pid = -1;
  char preload[PATH_MAX + 32];
  char **child_env =
      baseline_environment(argv[argc - 1], preload, sizeof preload);
  if (!child_env) {
    rc = 1;
  }
  if (!rc) {
    rc = posix_spawn(&pid, baseline[0], &actions, NULL, baseline, child_env);
  }

  free(child_env);
  if (initialized) {
    posix_spawn_file_actions_destroy(&actions);
  }
  free(baseline);

  int status = 0;
  int completed = !rc && wait_baseline(pid, &status);
  struct stat output = {0};
  int size_valid = fstat(fileno(log), &output) == 0 && output.st_size >= 0 &&
                   output.st_size <= 65536;
  /* The child uses the shared file offset through dup2; inspect its output
   * before allocating or printing it. No partial or oversized baseline passes.
   */
  if (!size_valid) {
    rc = 1;
  }
  int exit_code = completed && WIFEXITED(status)     ? WEXITSTATUS(status)
                  : completed && WIFSIGNALED(status) ? 128 + WTERMSIG(status)
                                                     : -1;

  if (size_valid) {
    rewind(log);
    printf("JRE_LAUNCHER_BASELINE_BEGIN\n");
    char buffer[4096];
    size_t n;
    while ((n = fread(buffer, 1, sizeof buffer, log))) {
      fwrite(buffer, 1, n, stdout);
    }
    printf("\nJRE_LAUNCHER_BASELINE_END exit=%d\n", exit_code);
  }

  if (rc || !completed || exit_code != 0 || !size_valid) {
    printf("JRE_LAUNCHER_BASELINE_ERROR spawn=%d completed=%d status=%d\n", rc,
           completed, status);
    rc = 1;
  }

  fclose(log);
  return rc;
}
#endif

int gw_test_pojav_jli(void *libjli, int argc, const char *const *arguments) {
  JLI_Launch_func launch = (void *)dlsym(libjli, "JLI_Launch");
  if (!launch || argc < 1) {
    printf("JRE_JLI_ERROR=missing JLI entry or arguments\n");
    return 1;
  }

  char **argv = calloc((size_t)argc + 1, sizeof *argv);
  char **owned = calloc((size_t)argc, sizeof *owned);
  int result = 1;
  if (!argv || !owned) {
    goto done;
  }

  for (int i = 0; i < argc; i++) {
    owned[i] = strdup(arguments[i]);
    if (!owned[i]) {
      goto done;
    }
    argv[i] = owned[i];
  }

#if JRE_LAUNCHER_JNI_BASELINE
  if (launcher_baseline(argc, argv)) {
    goto done;
  }
#endif

  /* Pojav supplies these legacy launcher labels even for newer target VMs;
   * they do not choose the actual VM version or our loader interface family. */
  result = launch(argc, argv, 0, NULL, 0, NULL, "1.8.0-internal", "1.8",
                  argv[0], "openjdk", JNI_FALSE, JNI_TRUE, JNI_FALSE, 0);

done:
  if (owned) {
    for (int i = 0; i < argc; i++) {
      free(owned[i]);
    }
  }
  free(owned);
  free(argv);
  return result;
}
