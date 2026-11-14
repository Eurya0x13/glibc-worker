#define _GNU_SOURCE
#include <jni.h>
#include <dlfcn.h>
#include <sqlite3.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static _Atomic int *destructors;

JNIEXPORT void conformance_observe_destructor(_Atomic int *p) {
  destructors = p;
}

__attribute__((destructor)) static void fini(void) {
  if (destructors) {
    atomic_fetch_add(destructors, 1);
  }
}

static int execute(sqlite3 *db, const char *sql) {
  char *error = NULL;
  int rc = sqlite3_exec(db, sql, NULL, NULL, &error);
  if (rc != SQLITE_OK) {
    dprintf(2, "SQLITE_ERROR rc=%d message=%s\n", rc,
            error ? error : sqlite3_errmsg(db));
  }

  sqlite3_free(error);
  return rc;
}

static int insert_range(sqlite3 *db, int first, int count) {
  int rc = execute(db, "BEGIN IMMEDIATE");
  if (rc) {
    return rc;
  }
  sqlite3_stmt *statement = NULL;
  rc = sqlite3_prepare_v2(db, "INSERT INTO samples(id,value) VALUES(?1,?2)", -1,
                          &statement, NULL);

  for (int n = first; rc == SQLITE_OK && n < first + count; n++) {
    sqlite3_bind_int(statement, 1, n);
    sqlite3_bind_int(statement, 2, n);
    int step = sqlite3_step(statement);
    if (step != SQLITE_DONE) {
      rc = step;
      break;
    }
    sqlite3_reset(statement);
  }
  sqlite3_finalize(statement);

  if (rc == SQLITE_OK) {
    rc = execute(db, "COMMIT");
  } else {
    execute(db, "ROLLBACK");
  }
  return rc;
}

struct job {
  const char *path;
  int first, rc;
};

static void *writer(void *opaque) {
  struct job *j = opaque;
  sqlite3 *db = NULL;
  j->rc = sqlite3_open_v2(j->path, &db,
                          SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, NULL);

  if (j->rc == SQLITE_OK) {
    sqlite3_busy_timeout(db, 10000);
    j->rc = insert_range(db, j->first, 25);
  }
  if (db) {
    sqlite3_close(db);
  }
  return NULL;
}

static void remove_database(const char *path) {
  char side[4096];
  unlink(path);

  snprintf(side, sizeof side, "%s-wal", path);
  unlink(side);
  snprintf(side, sizeof side, "%s-shm", path);
  unlink(side);
}

JNIEXPORT jstring JNICALL Java_multi_Sqlite_version(JNIEnv *e, jclass cls) {
  (void)cls;
  return (*e)->NewStringUTF(e, sqlite3_libversion());
}

JNIEXPORT jint JNICALL Java_multi_Sqlite_exercise(JNIEnv *e, jclass cls,
                                                  jstring path) {
  Dl_info sql_info = {0}, libc_info = {0};
  dladdr((void *)sqlite3_open_v2, &sql_info);
  dladdr((void *)malloc, &libc_info);
  dprintf(2, "SQLITE_RUNTIME pid=%d tid=%ld sqlite=%s libc=%s\n", getpid(),
          (long)gettid(), sql_info.dli_fname, libc_info.dli_fname);
  (void)cls;

  const char *p = (*e)->GetStringUTFChars(e, path, NULL);
  if (!p) {
    return -1;
  }
  char *copy = strdup(p);
  (*e)->ReleaseStringUTFChars(e, path, p);
  if (!copy) {
    return -2;
  }
  remove_database(copy);

  sqlite3 *db = NULL;
  sqlite3_stmt *query = NULL;
  int result = 1, rows = -1, sum = -1;
  int rc = sqlite3_open_v2(
      copy, &db,
      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL);
  if (rc != SQLITE_OK) {
    goto done;
  }
  sqlite3_busy_timeout(db, 10000);

  if (execute(db,
              "PRAGMA journal_mode=WAL;CREATE TABLE samples(id INTEGER PRIMARY "
              "KEY,value "
              "INTEGER NOT NULL)") != SQLITE_OK) {
    goto done;
  }
  /* Check the actual selected journal mode rather than assuming the PRAGMA
   * worked. */
  if (sqlite3_prepare_v2(db, "PRAGMA journal_mode", -1, &query, NULL) !=
          SQLITE_OK ||
      sqlite3_step(query) != SQLITE_ROW ||
      strcmp((const char *)sqlite3_column_text(query, 0), "wal")) {
    goto done;
  }
  sqlite3_finalize(query);
  query = NULL;
  if (insert_range(db, 0, 100) != SQLITE_OK) {
    goto done;
  }

  pthread_t threads[4];
  struct job jobs[4];
  int created = 0;
  for (int i = 0; i < 4; i++) {
    jobs[i] = (struct job){.path = copy, .first = 100 + i * 25, .rc = -1};
    if (pthread_create(&threads[i], NULL, writer, &jobs[i])) {
      break;
    }
    created++;
  }
  for (int i = 0; i < created; i++) {
    pthread_join(threads[i], NULL);
  }
  if (created != 4) {
    goto done;
  }
  for (int i = 0; i < 4; i++) {
    if (jobs[i].rc != SQLITE_OK) {
      goto done;
    }
  }

  if (sqlite3_prepare_v2(db, "PRAGMA integrity_check", -1, &query, NULL) !=
          SQLITE_OK ||
      sqlite3_step(query) != SQLITE_ROW ||
      strcmp((const char *)sqlite3_column_text(query, 0), "ok")) {
    goto done;
  }
  sqlite3_finalize(query);
  query = NULL;
  if (sqlite3_wal_checkpoint_v2(db, NULL, SQLITE_CHECKPOINT_FULL, NULL, NULL) !=
      SQLITE_OK) {
    goto done;
  }
  if (sqlite3_close(db) != SQLITE_OK) {
    goto done;
  }

  db = NULL;
  if (sqlite3_open_v2(copy, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
    goto done;
  }
  if (sqlite3_prepare_v2(db, "SELECT count(*),sum(value) FROM samples", -1,
                         &query, NULL) != SQLITE_OK ||
      sqlite3_step(query) != SQLITE_ROW) {
    goto done;
  }
  rows = sqlite3_column_int(query, 0);
  sum = sqlite3_column_int(query, 1);
  result = rows == 200 && sum == 19900 ? 0 : 2;

done:
  if (result) {
    dprintf(2, "SQLITE_EXERCISE_FAIL sqlite_rc=%d message=%s\n",
            db ? sqlite3_errcode(db) : rc,
            db ? sqlite3_errmsg(db) : "open failed");
  }
  if (query) {
    sqlite3_finalize(query);
  }
  if (db) {
    sqlite3_close(db);
  }

  dprintf(2,
          "SQLITE_RESULT version=%s threadsafe=%d writers=4 rows=%d sum=%d "
          "wal=%s integrity=%s "
          "reopened=%d result=%d\n",
          sqlite3_libversion(), sqlite3_threadsafe(), rows, sum,
          result ? "unverified" : "wal", result ? "unverified" : "ok", !result,
          result);

  remove_database(copy);
  free(copy);
  return result;
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *opaque) {
  (void)opaque;
  JNIEnv *e = NULL;

  return (*vm)->GetEnv(vm, (void **)&e, JNI_VERSION_1_6) == JNI_OK
             ? JNI_VERSION_1_6
             : JNI_ERR;
}

JNIEXPORT void JNICALL JNI_OnUnload(JavaVM *vm, void *opaque) {
  (void)vm;
  (void)opaque;
  dprintf(2, "SQLITE_SHUTDOWN rc=%d\n", sqlite3_shutdown());
}
