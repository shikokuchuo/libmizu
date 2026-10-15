/* Unit tier seed: the region layer, preamble validator, parker, and
   liveness lock, exercised in-process. Grows into the full unit tier
   (plan step 3). Assert-based; run via `make test`. */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "internal.h"

int main(void) {
  assert(mizu_version() != NULL);
  assert(mizu_type_elt_size(MIZU_TYPE_REAL) == 8);
  assert(mizu_type_elt_size(MIZU_TYPE_INT) == 4);
  assert(mizu_type_elt_size(MIZU_TYPE_INT64) == 8);
  assert(mizu_type_elt_size(MIZU_TYPE_STR) == 0);

  /* region create / open / round-trip / close */
  mizu_shm *shm = NULL;
  assert(mizu_shm_create(&shm, 4096) == MIZU_OK);
  assert(mizu_shm_size(shm) == 4096);
  memset(mizu_shm_addr(shm), 0x5A, 4096);

  mizu_shm *ro = NULL;
  assert(mizu_shm_open(&ro, mizu_shm_name(shm)) == MIZU_OK);
  assert(((unsigned char *) mizu_shm_addr(ro))[0] == 0x5A);
  mizu_shm_close(ro, 0);

  mizu_shm *rw = NULL;
  assert(mizu_shm_open_rw(&rw, mizu_shm_name(shm), 1) == MIZU_OK);
  assert(((unsigned char *) mizu_shm_addr(rw))[4095] == 0x5A);
  mizu_shm_close(rw, 0);

  /* a vanished name opens NULL with the error slot set (copy the name
     first — close frees the handle it points into) */
  mizu_shm *gone = NULL;
  char gone_name[MIZU_NAME_MAX];
  snprintf(gone_name, sizeof(gone_name), "%s", mizu_shm_name(shm));
  mizu_shm_close(shm, 1);
  assert(mizu_shm_open(&gone, gone_name) == MIZU_ERR);
  assert(mizu_last_error_category() != MIZU_ERRCAT_NONE);
  assert(mizu_last_error_message()[0] != '\0');

  /* preamble write + validate, incl. a corrupt-magic rejection (the
     region must hold the fixed layout plus both rings: 512 + 2*8*64) */
  mizu_shm *ctl = NULL;
  assert(mizu_shm_create(&ctl, 4096) == MIZU_OK);
  mizu_preamble p;
  memset(&p, 0, sizeof(p));
  p.magic = MIZU_MAGIC;
  p.version = MIZU_ABI_VERSION;
  p.cap = 8;
  p.slot = 64;
  mizu_preamble_write(mizu_shm_addr(ctl), &p);
  mizu_preamble out;
  assert(mizu_preamble_validate(mizu_shm_addr(ctl), mizu_shm_size(ctl),
                               &out) == NULL);
  assert(out.cap == 8 && out.slot == 64);
  memset(mizu_shm_addr(ctl), 0, 4);
  assert(mizu_preamble_validate(mizu_shm_addr(ctl), mizu_shm_size(ctl),
                               &out) != NULL);

  /* parker: an unpark between snapshot and park makes the park return
     WOKEN immediately (the lost-wakeup handshake) */
  mizu_parker pk;
  _Atomic uint32_t *epoch =
    (_Atomic uint32_t *) ((char *) mizu_shm_addr(ctl) +
                          MIZU_ENTITY_OFFSET(MIZU_ENTITY_HOST));
  assert(mizu_parker_attach(&pk, epoch, mizu_shm_name(ctl), MIZU_ENTITY_HOST,
                           1) == 0);
  uint32_t snap = mizu_parker_snapshot(&pk);
  mizu_unpark(&pk);
  assert(mizu_park(&pk, snap, 0) == MIZU_PARK_WOKEN);
  assert(mizu_park(&pk, mizu_parker_snapshot(&pk), 0) == MIZU_PARK_TIMEOUT);
  mizu_parker_detach(&pk);
  mizu_shm_close(ctl, 1);

  /* liveness: a held lock probes HELD on a second handle, ACQUIRED after
     the holder closes */
  const char *dir = mizu_live_dir();
  assert(dir != NULL);
  char path[1024];
  int pn = snprintf(path, sizeof(path), "%s/mizu_test_%ld.live", dir,
                    mizu_self_pid());
  assert(pn > 0 && (size_t) pn < sizeof(path));
  intptr_t h1, h2;
  assert(mizu_live_open(path, &h1) == 0);
  assert(mizu_live_try(h1) == MIZU_LIVE_ACQUIRED);
  assert(mizu_live_open_existing(path, &h2) == 0);
  assert(mizu_live_try(h2) == MIZU_LIVE_HELD);
  mizu_live_close(h1);
  assert(mizu_live_try(h2) == MIZU_LIVE_ACQUIRED);
  mizu_live_unlock(h2);
  mizu_live_close(h2);
  remove(path);

#ifndef _WIN32
  /* with stdin closed, a lock fd must bounce off 0 (the consumers'
     not-open sentinel) and close cleanly */
  {
    int saved = dup(0);
    close(0);
    assert(mizu_live_open(path, &h1) == 0);
    assert(h1 > 0);
    assert(fcntl(0, F_GETFD) == -1);   /* fd 0 stays free, not the lock */
    mizu_live_close(h1);
    assert(mizu_live_open_existing(path, &h2) == 0);
    assert(h2 > 0);
    mizu_live_close(h2);
    remove(path);
    if (saved >= 0) {
      dup2(saved, 0);
      close(saved);
    }
  }
#endif

#ifndef _WIN32
  /* with stdout/stderr closed, a lock fd bounces off 1/2 as well: a
     later dup2 / freopen onto it would release the flock (a false DEAD) */
  {
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
      intptr_t h;
      close(1);
      if (mizu_live_open(path, &h) != 0 || h <= 2) _exit(1);
      if (fcntl(1, F_GETFD) != -1) _exit(2);   /* fd 1 stays free */
      mizu_live_close(h);
      close(2);
      if (mizu_live_open_existing(path, &h) != 0 || h <= 2) _exit(3);
      if (fcntl(2, F_GETFD) != -1) _exit(4);   /* fd 2 stays free */
      mizu_live_close(h);
      _exit(0);
    }
    int st;
    assert(waitpid(pid, &st, 0) == pid);
    assert(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    remove(path);
  }

  /* a fork-without-exec child must not keep a dead holder's lock alive:
     the holder forks a lingering grandchild and exits; the atfork
     handler closed the grandchild's inherited fd at fork, so the probe
     acquires as soon as the holder dies — not when the grandchild
     finally exits */
  {
    char hpath[1100];
    int hn = snprintf(hpath, sizeof(hpath), "%s/mizu_test_fork_%ld.live",
                      dir, mizu_self_pid());
    assert(hn > 0 && (size_t) hn < sizeof(hpath));
    int pipefd[2];
    assert(pipe(pipefd) == 0);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
      close(pipefd[0]);
      intptr_t h;
      if (mizu_live_open(hpath, &h) != 0 ||
          mizu_live_try(h) != MIZU_LIVE_ACQUIRED)
        _exit(2);
      if (write(pipefd[1], "1", 1) != 1) _exit(3);   /* the lock is held */
      if (fork() == 0) {
        sleep(2);                                    /* outlives the holder */
        _exit(0);
      }
      _exit(0);                                      /* the holder dies */
    }
    close(pipefd[1]);
    char c;
    assert(read(pipefd[0], &c, 1) == 1);             /* wait for the lock */
    close(pipefd[0]);
    int st;
    assert(waitpid(pid, &st, 0) == pid);             /* holder reaped */
    intptr_t h2;
    assert(mizu_live_open_existing(hpath, &h2) == 0);
    assert(mizu_live_try(h2) == MIZU_LIVE_ACQUIRED); /* not HELD by the child */
    mizu_live_unlock(h2);
    mizu_live_close(h2);
    remove(hpath);
  }
#endif

  /* rng jump: deterministic, and changes the state */
  int seed[6] = { 1, 2, 3, 4, 5, 6 };
  int copy[6];
  memcpy(copy, seed, sizeof(seed));
  mizu_rng_jump(seed);
  assert(memcmp(copy, seed, sizeof(seed)) != 0);

  puts("test_region: ok");
  return 0;
}
