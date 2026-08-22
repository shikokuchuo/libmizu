/* Orphan reaping (shm.c): a child creates a region and exits without
   teardown, leaving the region — and on macOS its registry log — behind.
   The parent's rei_shm_reap must report the name, unlink the region,
   and drop the dead process's log. Exercises the Linux /dev/shm scan
   and the macOS log scan; other platforms have no enumerable namespace.
   Run via `make test`. */

/* _GNU_SOURCE: glibc hides O_CLOEXEC, MAP_POPULATE, pidfd_open, ...
   under strict -std=c11; define before any system header. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE
#endif

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "internal.h"

#if defined(__linux__) || defined(__APPLE__)

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

int main(void) {
  /* Scratch registry dir: the macOS log resolves $TMPDIR per call. */
  char scratch[PATH_MAX];
  int sn = snprintf(scratch, sizeof(scratch), "/tmp/rei_test_reap_%ld",
                    (long) getpid());
  assert(sn > 0 && (size_t) sn < sizeof(scratch));
  assert(mkdir(scratch, 0700) == 0);
  assert(setenv("TMPDIR", scratch, 1) == 0);

  int pipefd[2];
  assert(pipe(pipefd) == 0);

  pid_t pid = fork();
  assert(pid >= 0);
  if (pid == 0) {
    close(pipefd[0]);
    rei_shm *shm = NULL;
    if (rei_shm_create(&shm, 4096) == REI_OK) {
      size_t n = strlen(rei_shm_name(shm)) + 1;
      ssize_t w = write(pipefd[1], rei_shm_name(shm), n);
      (void) w;
    }
    _exit(0);                  /* no teardown: an orphan region (+ log) */
  }
  close(pipefd[1]);

  char name[REI_NAME_MAX];
  ssize_t got = read(pipefd[0], name, sizeof(name) - 1);
  close(pipefd[0]);
  assert(got > 0 && name[got - 1] == '\0');

  int st;
  assert(waitpid(pid, &st, 0) == pid);     /* no zombie: the pid reads dead */

#ifdef __APPLE__
  /* The child's log exists and names the orphan. */
  char logpath[PATH_MAX];
  int ln = snprintf(logpath, sizeof(logpath), "%s/rei/rei_%x",
                    scratch, (unsigned) pid);
  assert(ln > 0 && (size_t) ln < sizeof(logpath));
  assert(access(logpath, F_OK) == 0);
#endif

  int n = 0;
  char **reaped = rei_shm_reap(&n);
  int found = 0;
  for (int i = 0; i < n; i++) {
    if (strcmp(reaped[i], name) == 0) found = 1;
    free(reaped[i]);
  }
  free(reaped);
  assert(found);

  /* The region is gone. */
  rei_shm *gone = NULL;
  assert(rei_shm_open(&gone, name) == REI_ERR);

#ifdef __APPLE__
  /* The dead process's log is gone too, and the registry dir pruned. */
  errno = 0;
  assert(access(logpath, F_OK) != 0 && errno == ENOENT);
  char regdir[PATH_MAX];
  int rn = snprintf(regdir, sizeof(regdir), "%s/rei", scratch);
  assert(rn > 0 && (size_t) rn < sizeof(regdir));
  errno = 0;
  assert(access(regdir, F_OK) != 0 && errno == ENOENT);
#endif

  assert(rmdir(scratch) == 0);
  puts("test_reap: ok");
  return 0;
}

#else

int main(void) {
  puts("test_reap: ok (no enumerable SHM namespace)");
  return 0;
}

#endif
