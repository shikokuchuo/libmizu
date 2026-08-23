/* The macOS registry log under concurrent region creation (shm.c):
   threaded consumers create regions concurrently — a supported pattern
   — and the lock-free append path must serialize one 4-byte record per
   create (O_APPEND) with none torn or lost; the zero-crossing truncate
   then empties the log as the last region comes down. __APPLE__-only:
   the log exists nowhere else. Run via `make test`. */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "internal.h"

#ifdef __APPLE__

#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

enum { THREADS = 8, PER_THREAD = 64, TOTAL = THREADS * PER_THREAD };

static rei_shm *regions[TOTAL];

static void *worker(void *arg) {
  size_t base = (size_t) (uintptr_t) arg * PER_THREAD;
  for (int i = 0; i < PER_THREAD; i++)
    assert(rei_shm_create(&regions[base + i], 4096) == REI_OK);
  return NULL;
}

static long log_size(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 ? (long) st.st_size : -1;
}

static int cmp_u32(const void *a, const void *b) {
  uint32_t x = *(const uint32_t *) a, y = *(const uint32_t *) b;
  return (x > y) - (x < y);
}

int main(void) {
  /* Scratch registry dir: the log resolves $TMPDIR per call. */
  char scratch[PATH_MAX];
  int sn = snprintf(scratch, sizeof(scratch), "/tmp/rei_test_reg_%ld",
                    (long) getpid());
  assert(sn > 0 && (size_t) sn < sizeof(scratch));
  assert(mkdir(scratch, 0700) == 0);
  assert(setenv("TMPDIR", scratch, 1) == 0);

  pthread_t th[THREADS];
  for (long i = 0; i < THREADS; i++)
    assert(pthread_create(&th[i], NULL, worker, (void *) (uintptr_t) i) == 0);
  for (int i = 0; i < THREADS; i++)
    assert(pthread_join(th[i], NULL) == 0);

  /* One whole 4-byte record per create, all counters distinct. */
  char logpath[PATH_MAX];
  int ln = snprintf(logpath, sizeof(logpath), "%s/rei/rei_%x",
                    scratch, (unsigned) getpid());
  assert(ln > 0 && (size_t) ln < sizeof(logpath));
  assert(log_size(logpath) == (long) TOTAL * 4);
  FILE *f = fopen(logpath, "rb");
  assert(f != NULL);
  uint32_t recs[TOTAL];
  assert(fread(recs, sizeof(uint32_t), TOTAL, f) == TOTAL);
  fclose(f);
  qsort(recs, TOTAL, sizeof(uint32_t), cmp_u32);
  for (int i = 1; i < TOTAL; i++) assert(recs[i] != recs[i - 1]);

  /* Tearing down the last region truncates the log in place (512
     records clear the 256-record floor). */
  for (int i = 0; i < TOTAL; i++) rei_shm_close(regions[i], 1);
  assert(log_size(logpath) == 0);

  /* Appends continue from the truncated end; below the floor the
     zero-crossing leaves the record be. */
  rei_shm *one = NULL;
  assert(rei_shm_create(&one, 4096) == REI_OK);
  assert(log_size(logpath) == 4);
  rei_shm_close(one, 1);
  assert(log_size(logpath) == 4);

  /* The exit/unload teardown is a no-op while a region is live... */
  rei_shm *two = NULL;
  assert(rei_shm_create(&two, 4096) == REI_OK);
  rei_log_teardown();
  assert(log_size(logpath) == 8);

  /* ...and removes the log and prunes the dir once the last region
     comes down (in production the reaper owns dead processes' logs). */
  rei_shm_close(two, 1);
  rei_log_teardown();
  assert(log_size(logpath) == -1);
  char regdir[PATH_MAX];
  int rn = snprintf(regdir, sizeof(regdir), "%s/rei", scratch);
  assert(rn > 0 && (size_t) rn < sizeof(regdir));
  struct stat st;
  assert(stat(regdir, &st) != 0);

  assert(rmdir(scratch) == 0);

  puts("test_registry: ok");
  return 0;
}

#else

int main(void) {
  puts("test_registry: ok (log is __APPLE__-only)");
  return 0;
}

#endif
