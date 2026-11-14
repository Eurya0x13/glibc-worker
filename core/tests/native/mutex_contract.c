/* Test the actual linked libc. No mutex layout or futex-word assumptions. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "mutex_contract.h"

static int checks, failures, rejected, accepted, owner_dead, stuck;

static void check(int good, const char *name, int result) {
  checks++;

  if (!good) {
    failures++;
    dprintf(STDOUT_FILENO, "MUTEX_FAILURE name=%s rc=%d\n", name, result);
  }
}

static struct timespec deadline(void) {
  struct timespec value;
  clock_gettime(CLOCK_REALTIME, &value);

  value.tv_nsec += 100000000;
  if (value.tv_nsec >= 1000000000) {
    value.tv_sec++;
    value.tv_nsec -= 1000000000;
  }

  return value;
}

struct owner {
  pthread_mutex_t *mutex;
  int result;
};

static void *leave_locked(void *opaque) {
  struct owner *owner = opaque;
  owner->result = pthread_mutex_lock(owner->mutex);
  return NULL; /* Exercise ordinary pthread cleanup, not a raw SYS_exit. */
}

static void *timed_contender(void *opaque) {
  struct owner *owner = opaque;
  struct timespec until = deadline();

  owner->result = pthread_mutex_timedlock(owner->mutex, &until);
  if (!owner->result) {
    pthread_mutex_unlock(owner->mutex);
  }
  return NULL;
}

static void recovery(pthread_mutex_t *mutex, int inconsistent) {
  struct owner owner = {.mutex = mutex, .result = -1};
  pthread_t thread;
  int rc = pthread_create(&thread, NULL, leave_locked, &owner);
  check(rc == 0, "owner-create", rc);
  if (rc) {
    return;
  }

  rc = pthread_join(thread, NULL);
  check(rc == 0 && owner.result == 0, "owner-exited-locked", rc);
  if (rc || owner.result) {
    return;
  }

  struct timespec until = deadline();
  rc = pthread_mutex_timedlock(mutex, &until);
  owner_dead += rc == EOWNERDEAD;
  stuck += rc == ETIMEDOUT;
  check(rc == EOWNERDEAD, "owner-death", rc);
  if (rc != EOWNERDEAD) {
    if (!rc) {
      pthread_mutex_unlock(mutex);
    }
    return;
  }

  if (!inconsistent) {
    rc = pthread_mutex_consistent(mutex);
    check(rc == 0, "consistent", rc);
  }

  rc = pthread_mutex_unlock(mutex);
  check(rc == 0, "recovered-unlock", rc);
  rc = pthread_mutex_trylock(mutex);
  check(rc == (inconsistent ? ENOTRECOVERABLE : 0), "recovery-state", rc);
  if (!rc) {
    pthread_mutex_unlock(mutex);
  }
}

static void robust(enum gw_mutex_expectation expected) {
  const int types[] = {PTHREAD_MUTEX_NORMAL, PTHREAD_MUTEX_RECURSIVE,
                       PTHREAD_MUTEX_ERRORCHECK};
  const int sharing[] = {PTHREAD_PROCESS_PRIVATE, PTHREAD_PROCESS_SHARED};
  const int protocols[] = {PTHREAD_PRIO_NONE, PTHREAD_PRIO_INHERIT};
  for (unsigned t = 0; t < sizeof types / sizeof *types; t++) {
    for (unsigned s = 0; s < sizeof sharing / sizeof *sharing; s++) {
      for (unsigned p = 0; p < sizeof protocols / sizeof *protocols; p++) {
        pthread_mutexattr_t attr;
        int rc = pthread_mutexattr_init(&attr);
        check(!rc, "attr-init", rc);
        if (rc) {
          return;
        }

        rc = pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
        check(!rc, "attr-robust", rc);
        rc = pthread_mutexattr_settype(&attr, types[t]);
        check(!rc, "attr-type", rc);
        rc = pthread_mutexattr_setpshared(&attr, sharing[s]);
        check(!rc, "attr-sharing", rc);
        rc = pthread_mutexattr_setprotocol(&attr, protocols[p]);
        check(!rc, "attr-protocol", rc);

        pthread_mutex_t mutex;
        unsigned char before[sizeof mutex];
        memset(&mutex, 0x5a, sizeof mutex);
        memcpy(before, &mutex, sizeof mutex);
        rc = pthread_mutex_init(&mutex, &attr);
        rejected += rc == ENOTSUP;
        accepted += rc == 0;
        if (expected == GW_MUTEX_REJECTED) {
          check(rc == ENOTSUP, "robust-init-rejected", rc);
          if (rc == ENOTSUP) {
            check(!memcmp(before, &mutex, sizeof mutex), "reject-before-write",
                  rc);
          }
        } else {
          check(rc == 0, "reference-robust-init", rc);
        }

        /* A private non-PI mutex provides a bounded old-runtime reproducer.
           The shared tests use attributes only; they do not claim IPC support.
         */
        if (!rc) {
          if (!t && !s && !p) {
            recovery(&mutex, 0);
          }
          int destroyed = pthread_mutex_destroy(&mutex);
          check(destroyed == 0 || (stuck && destroyed == EBUSY), "destroy",
                destroyed);
        }

        rc = pthread_mutexattr_destroy(&attr);
        check(!rc, "attr-destroy", rc);
      }
    }
  }

  if (expected == GW_MUTEX_RECOVERED) {
    pthread_mutexattr_t attr;
    pthread_mutex_t mutex;
    int rc = pthread_mutexattr_init(&attr);
    check(!rc, "inconsistent-attr", rc);
    rc = pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
    check(!rc, "inconsistent-robust", rc);
    rc = pthread_mutex_init(&mutex, &attr);
    check(!rc, "inconsistent-init", rc);

    if (!rc) {
      recovery(&mutex, 1);
      rc = pthread_mutex_destroy(&mutex);
      check(!rc, "inconsistent-destroy", rc);
    }
    pthread_mutexattr_destroy(&attr);
  }
}

struct waiter {
  pthread_mutex_t mutex;
  pthread_cond_t condition;
  _Atomic int entered;
  int release, result;
};

static void *wait_condition(void *opaque) {
  struct waiter *waiter = opaque;
  waiter->result = pthread_mutex_lock(&waiter->mutex);
  if (waiter->result) {
    return NULL;
  }

  atomic_store_explicit(&waiter->entered, 1, memory_order_release);
  struct timespec until = deadline();
  until.tv_sec += 2;

  while (!waiter->release && !waiter->result) {
    waiter->result =
        pthread_cond_timedwait(&waiter->condition, &waiter->mutex, &until);
  }

  pthread_mutex_unlock(&waiter->mutex);
  return NULL;
}

static void ordinary(void) {
  const int types[] = {PTHREAD_MUTEX_NORMAL, PTHREAD_MUTEX_RECURSIVE,
                       PTHREAD_MUTEX_ERRORCHECK};
  for (unsigned i = 0; i < sizeof types / sizeof *types; i++) {
    pthread_mutexattr_t attr;
    pthread_mutex_t mutex;
    int rc = pthread_mutexattr_init(&attr);
    check(!rc, "ordinary-attr", rc);
    rc = pthread_mutexattr_settype(&attr, types[i]);
    check(!rc, "ordinary-type", rc);
    rc = pthread_mutex_init(&mutex, &attr);
    check(!rc, "ordinary-init", rc);
    if (rc) {
      pthread_mutexattr_destroy(&attr);
      continue;
    }

    rc = pthread_mutex_lock(&mutex);
    check(!rc, "ordinary-lock", rc);
    if (!rc) {
      if (types[i] == PTHREAD_MUTEX_RECURSIVE) {
        rc = pthread_mutex_lock(&mutex);
        check(!rc, "recursive-relock", rc);
        if (!rc) {
          pthread_mutex_unlock(&mutex);
        }
      } else if (types[i] == PTHREAD_MUTEX_ERRORCHECK) {
        rc = pthread_mutex_lock(&mutex);
        check(rc == EDEADLK, "errorcheck-relock", rc);
      } else {
        rc = pthread_mutex_trylock(&mutex);
        check(rc == EBUSY, "normal-busy", rc);
        struct owner contender = {.mutex = &mutex, .result = -1};
        pthread_t thread;
        rc = pthread_create(&thread, NULL, timed_contender, &contender);
        check(!rc, "contender-create", rc);
        if (!rc) {
          rc = pthread_join(thread, NULL);
          check(!rc && contender.result == ETIMEDOUT, "contention-timeout",
                contender.result);
        }
      }
      rc = pthread_mutex_unlock(&mutex);
      check(!rc, "ordinary-unlock", rc);
    }

    rc = pthread_mutex_destroy(&mutex);
    check(!rc, "ordinary-destroy", rc);
    pthread_mutexattr_destroy(&attr);
  }

  struct waiter waiter = {.mutex = PTHREAD_MUTEX_INITIALIZER,
                          .condition = PTHREAD_COND_INITIALIZER};
  pthread_t thread;
  int rc = pthread_create(&thread, NULL, wait_condition, &waiter);
  check(!rc, "waiter-create", rc);
  if (rc) {
    return;
  }

  struct timespec until = deadline();
  until.tv_sec += 1;
  struct timespec now;
  while (!atomic_load_explicit(&waiter.entered, memory_order_acquire)) {
    clock_gettime(CLOCK_REALTIME, &now);
    if (now.tv_sec > until.tv_sec ||
        (now.tv_sec == until.tv_sec && now.tv_nsec >= until.tv_nsec)) {
      break;
    }
    usleep(1000);
  }

  rc = pthread_mutex_lock(&waiter.mutex);
  check(!rc, "condition-lock", rc);
  waiter.release = 1;
  rc = pthread_cond_signal(&waiter.condition);
  check(!rc, "condition-signal", rc);
  pthread_mutex_unlock(&waiter.mutex);

  rc = pthread_join(thread, NULL);
  check(!rc && !waiter.result && atomic_load(&waiter.entered), "condition-join",
        rc);
  rc = pthread_cond_destroy(&waiter.condition);
  check(!rc, "condition-destroy", rc);
  rc = pthread_mutex_destroy(&waiter.mutex);
  check(!rc, "condition-mutex-destroy", rc);
}

int gw_mutex_contract(enum gw_mutex_expectation expected) {
  checks = failures = rejected = accepted = owner_dead = stuck = 0;

  ordinary();
  robust(expected);

  dprintf(STDOUT_FILENO,
          "MUTEX_CONTRACT {\"result\":\"%s\",\"policy\":\"%s\",\"checks\":%d,"
          "\"failures\":%d,\"robust_rejected\":%d,\"robust_accepted\":%d,"
          "\"owner_dead\":%d,\"owner_timed_out\":%d}\n",
          failures ? "FAIL" : "PASS",
          expected == GW_MUTEX_REJECTED ? "reject" : "recover", checks,
          failures, rejected, accepted, owner_dead, stuck);
  return failures ? -1 : 0;
}

#ifdef GW_MUTEX_STANDALONE
int main(int argc, char **argv) {
  return gw_mutex_contract(argc == 2 && !strcmp(argv[1], "--reject")
                               ? GW_MUTEX_REJECTED
                               : GW_MUTEX_RECOVERED) != 0;
}
#endif
