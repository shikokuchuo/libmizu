/* Unit tier seed: the region layer, preamble validator, parker, and
   liveness lock, exercised in-process. Grows into the full unit tier
   (plan step 3). Assert-based; run via `make test`. */

/* _GNU_SOURCE: glibc hides nanosleep and other POSIX declarations under
   strict -std=c11; define before any system header. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE
#endif

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif

#include "internal.h"

#ifndef _WIN32
/* The liveness dir resolution is thread-local: a fresh thread re-resolves
   it (TMPDIR unset falls back to the platform default; an oversized
   TMPDIR fails the resolution, refusing the creates). */
struct live_dir_args {
  const char *tmpdir;   /* NULL unsets */
  const char *dir;
  mizu_status ch_rc, pl_rc, sh_rc;
  char ch_msg[128], pl_msg[128];
};

static void *live_dir_main(void *arg) {
  struct live_dir_args *a = arg;
  if (a->tmpdir == NULL)
    unsetenv("TMPDIR");
  else
    setenv("TMPDIR", a->tmpdir, 1);
  a->dir = mizu_live_dir();
  if (a->dir == NULL) {
    /* a failed resolution refuses the creates through the TLS slot */
    mizu_binding b;
    mizu_binding_bytes(&b);
    mizu_channel_opts co;
    mizu_channel_opts_init(&co);
    mizu_channel *c = NULL;
    a->ch_rc = mizu_channel_create(&c, &co, &b);
    snprintf(a->ch_msg, sizeof(a->ch_msg), "%s", mizu_last_error_message());
    mizu_pool_opts po;
    mizu_pool_opts_init(&po);
    po.max_workers = 1;
    mizu_pool *p = NULL;
    a->pl_rc = mizu_pool_create(&p, &po, &b);
    snprintf(a->pl_msg, sizeof(a->pl_msg), "%s", mizu_last_error_message());
  }
  /* a region create resolves the registry log's dir on this thread too */
  mizu_shm *s = NULL;
  a->sh_rc = mizu_shm_create(&s, 4096);
  mizu_shm_close(s, a->sh_rc == MIZU_OK);
  return NULL;
}

/* A signal to the parked thread breaks the sleep as PARK_INTR. */
static mizu_parker intr_pk;
static void eintr_handler(int sig) {
  (void) sig;
}
static void *eintr_park_main(void *arg) {
  (void) arg;
  uint32_t e = mizu_parker_snapshot(&intr_pk);
  return (void *) (intptr_t) mizu_park(&intr_pk, e, 30000);
}

/* An unpark on a short delay: the indefinite park wakes from its sleep,
   never waiting out the nominal bound. */
static void *delayed_unpark(void *arg) {
  mizu_parker *pk = arg;
  struct timespec ts = { 0, 1000000 };
  nanosleep(&ts, NULL);
  mizu_unpark(pk);
  return NULL;
}
#endif

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

#ifndef _WIN32
  /* a zero-length object: the create-side mmap fails (the create-fail
     walk-back), and both consumer opens fail the same way */
  {
    mizu_shm z;
    assert(mizu_shm_create_stack(&z, 0) != MIZU_ERRCAT_NONE);
    char zname[MIZU_NAME_MAX];
    snprintf(zname, sizeof(zname), "/mizu_zl_%ld", (long) getpid());
#ifdef __linux__
    char zpath[64];
    snprintf(zpath, sizeof(zpath), "/dev/shm%s", zname);
    int zfd = open(zpath, O_CREAT | O_RDWR | O_TRUNC, 0600);
#else
    int zfd = shm_open(zname, O_CREAT | O_RDWR | O_TRUNC, 0600);
#endif
    assert(zfd >= 0);
    assert(close(zfd) == 0);
    assert(mizu_shm_open_stack(&z, zname) != 0);
    assert(mizu_shm_open_rw_stack(&z, zname, 0) != 0);
#ifdef __linux__
    assert(unlink(zpath) == 0);
#else
    assert(shm_unlink(zname) == 0);
#endif
  }
#endif

  /* a create reserves every page up front: write a pattern across the
     whole region, read it back through a second mapping */
  {
    const size_t n = 8 * 1024 * 1024;
    mizu_shm *w = NULL;
    assert(mizu_shm_create(&w, n) == MIZU_OK);
    unsigned char *wp = mizu_shm_addr(w);
    for (size_t i = 0; i < n; i += 4096) wp[i] = (unsigned char) (i >> 12);
    mizu_shm *r = NULL;
    assert(mizu_shm_open(&r, mizu_shm_name(w)) == MIZU_OK);
    const unsigned char *rp = mizu_shm_addr(r);
    for (size_t i = 0; i < n; i += 4096)
      assert(rp[i] == (unsigned char) (i >> 12));
    mizu_shm_close(r, 0);
    mizu_shm_close(w, 1);
  }

#ifdef __linux__
  /* an oversize create fails fast with NOSPACE and materializes nothing:
     the statvfs availability probe rejects ahead of the reservation,
     which would otherwise grind page-by-page through all of /dev/shm
     (minutes on a large tmpfs — the generous bound below catches a
     regression to the grind; the probe itself is microseconds) */
  {
    struct statvfs sv;
    assert(statvfs("/dev/shm", &sv) == 0);
    uint64_t avail = (uint64_t) sv.f_bavail * sv.f_frsize;
    struct timespec t0, t1;
    assert(clock_gettime(CLOCK_MONOTONIC, &t0) == 0);
    mizu_shm big;
    assert(mizu_shm_create_stack(&big, (size_t) 1 << 52) ==
           MIZU_ERRCAT_NOSPACE);
    assert(clock_gettime(CLOCK_MONOTONIC, &t1) == 0);
    assert(t1.tv_sec - t0.tv_sec < 10);
    assert(statvfs("/dev/shm", &sv) == 0);
    assert((uint64_t) sv.f_bavail * sv.f_frsize == avail);
  }
#endif

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
#ifndef _WIN32
  /* an indefinite park (timeout < 0) sleeps for the nominal bound unless
     unparked — never the untimed-wait swallow */
  pthread_t un;
  assert(pthread_create(&un, NULL, delayed_unpark, &pk) == 0);
  assert(mizu_park(&pk, mizu_parker_snapshot(&pk), -1) == MIZU_PARK_WOKEN);
  assert(pthread_join(un, NULL) == 0);
#endif
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
    /* the grandchild's atfork handler runs in its own context: on a
       loaded machine it may not have been scheduled yet — poll instead
       of demanding an immediate verdict */
    int acquired = 0;
    for (int i = 0; i < 5000 && !acquired; i++) {
      acquired = mizu_live_try(h2) == MIZU_LIVE_ACQUIRED;
      if (!acquired) {
        struct timespec ts = { 0, 1000000 };
        nanosleep(&ts, NULL);
      }
    }
    assert(acquired);                                /* not HELD by the child */
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

  /* err_describe: a summary per category, a hint only where one helps */
  {
    const char *summary, *hint;
    mizu_err_describe(MIZU_ERRCAT_NOSPACE, &summary, &hint);
    assert(strstr(summary, "space") != NULL && hint[0] != '\0');
    mizu_err_describe(MIZU_ERRCAT_NOMEMORY, &summary, &hint);
    assert(strstr(summary, "memory") != NULL && hint[0] == '\0');
    mizu_err_describe(MIZU_ERRCAT_EXISTS, &summary, &hint);
    assert(strstr(summary, "already in use") != NULL && hint[0] != '\0');
    mizu_err_describe(MIZU_ERRCAT_OTHER, &summary, &hint);
    assert(strstr(summary, "unexpected") != NULL && hint[0] == '\0');
  }

  /* the writable / view opens: a vanished name errors with the TLS slot
     set; a malformed view name is refused before the open */
  {
    mizu_shm *no = NULL;
    char no_name[64];
    int nn = snprintf(no_name, sizeof(no_name), "%s0_0", MIZU_PREFIX_LITERAL);
    assert(nn > 0 && (size_t) nn < sizeof(no_name));
    assert(mizu_shm_open_rw(&no, no_name, 0) == MIZU_ERR);
    assert(strstr(mizu_last_error_message(), "cannot open region") != NULL);
    char mal[64];
    nn = snprintf(mal, sizeof(mal), "%s../x", MIZU_PREFIX_LITERAL);
    assert(nn > 0 && (size_t) nn < sizeof(mal));
    assert(mizu_shm_open_view_flags(&no, mal, 0) == MIZU_ERR);
    assert(strstr(mizu_last_error_message(), "malformed region name") !=
           NULL);
    assert(mizu_shm_open_view_flags(&no, no_name, 0) == MIZU_ERR);
    assert(strstr(mizu_last_error_message(), "cannot open region") != NULL);
  }

  /* the view open: the default form performs the counted add, NOCOUNT
     leaves it to the caller */
  {
    mizu_shm *vreg = NULL;
    assert(mizu_shm_create(&vreg, 4096) == MIZU_OK);
    mizu_shm *v = NULL;
    assert(mizu_shm_open_view(&v, mizu_shm_name(vreg)) == MIZU_OK);
    assert(mizu_zc_refcount(v) == 1);
    mizu_zc_unref(v);
    assert(mizu_zc_refcount(v) == 0);
    mizu_shm_close(v, 0);
    assert(mizu_shm_open_view_flags(&v, mizu_shm_name(vreg),
                                    MIZU_OPEN_VIEW_NOCOUNT) == MIZU_OK);
    assert(mizu_zc_refcount(v) == 0);
    mizu_zc_ref(v);
    assert(mizu_zc_refcount(v) == 1);
    mizu_zc_unref(v);
    mizu_shm_close(v, 0);
    mizu_shm_close(vreg, 1);
  }

  /* the spin learner: a completed wait inside the cap grows 1.5x +
     headroom (never below the base, never past the cap); a wait past the
     cap halves toward the floor */
  {
    assert(mizu_spin_learn(20000.0, 16000, 16000) == 38000);
    assert(mizu_spin_learn(1.0, 16000, 16000) == 16000);
    assert(mizu_spin_learn(64000.0, 16000, 16000) == 64000);
    assert(mizu_spin_learn(1e9, 64000, 16000) == 32000);
    assert(mizu_spin_learn(1e9, 1600, 16000) == 1000);
  }

  /* the preamble validator's reject matrix (the happy path and the
     slot-range mirror live in the channel tier) */
  {
    mizu_preamble p2;
    memset(&p2, 0, sizeof(p2));
    p2.magic = MIZU_MAGIC;
    p2.version = MIZU_ABI_VERSION;
    p2.cap = 8;
    p2.slot = 64;
    size_t region_size = MIZU_FIXED_LAYOUT_SIZE + 2 * 8 * 64;
    assert(mizu_preamble_validate(&p2, MIZU_FIXED_LAYOUT_SIZE - 1,
                                  NULL) != NULL);
    p2.version += 1;
    assert(mizu_preamble_validate(&p2, region_size, NULL) != NULL);
    p2.version -= 1;
    p2.cap = 3;
    assert(mizu_preamble_validate(&p2, region_size, NULL) != NULL);
    p2.cap = 64;          /* 64*64 of rings past half the spare extent */
    assert(mizu_preamble_validate(&p2, region_size, NULL) != NULL);
    p2.cap = 8;
    p2.arena_size = 32;
    assert(mizu_preamble_validate(&p2, region_size, NULL) != NULL);
    p2.arena_size = 64;
    assert(mizu_preamble_validate(&p2, region_size, NULL) != NULL);
    p2.arena_size = 4096;   /* rings + arenas exceed the region */
    assert(mizu_preamble_validate(&p2, region_size, NULL) != NULL);
    p2.arena_size = 0;
    p2.drop_offset = region_size + 64;
    assert(mizu_preamble_validate(&p2, region_size, NULL) != NULL);
    p2.drop_offset = 0;
    p2.livedir_offset = region_size + 64;
    assert(mizu_preamble_validate(&p2, region_size, NULL) != NULL);
    p2.livedir_offset = 0;
    assert(mizu_preamble_validate(&p2, region_size, NULL) == NULL);
  }

  /* the death-listener teardown is callable (a no-op where the platform
     needs none) */
  mizu_death_listener_teardown();

#ifdef __linux__
  /* the listener restarts after a teardown: a fresh watch fires on the
     watched pid's exit, and a pre-teardown (now unlisted) watch's stop
     frees directly */
  {
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
      sleep(30);
      _exit(0);
    }
    _Atomic int dl_flag = 0;
    mizu_death_watch *w1 = mizu_death_watch_start((long) pid, &dl_flag, NULL);
    assert(w1 != NULL);
    mizu_death_listener_teardown();
    mizu_death_watch *w2 = mizu_death_watch_start((long) pid, &dl_flag, NULL);
    assert(w2 != NULL);
    kill(pid, SIGKILL);
    int wst;
    assert(waitpid(pid, &wst, 0) == pid);
    for (int i = 0; i < 5000 && !dl_flag; i++) {
      struct timespec ts = {0, 1000000};
      nanosleep(&ts, NULL);
    }
    assert(dl_flag);
    mizu_death_watch_stop(w1);
    mizu_death_watch_stop(w2);
    mizu_death_listener_teardown();
    puts("ok death listener restart");
  }
#endif

#ifndef _WIN32
  /* the liveness dir resolution is per-thread: on a fresh thread an
     oversized TMPDIR fails it (the creates refuse), an unset TMPDIR
     falls back to the platform default */
  {
    char saved_tmp[1100];
    const char *tmp0 = getenv("TMPDIR");
    snprintf(saved_tmp, sizeof(saved_tmp), "%s", tmp0 != NULL ? tmp0 : "");

    struct live_dir_args a;
    pthread_t th;
#ifndef __linux__
    /* an oversized TMPDIR fails the resolution (Linux never consults
       TMPDIR: the liveness dir is always /dev/shm) */
    char longtmp[2000];
    memset(longtmp, 't', sizeof(longtmp) - 1);
    longtmp[sizeof(longtmp) - 1] = '\0';
    memset(&a, 0, sizeof(a));
    a.tmpdir = longtmp;
    assert(pthread_create(&th, NULL, live_dir_main, &a) == 0);
    assert(pthread_join(th, NULL) == 0);
    assert(a.dir == NULL);
    assert(a.ch_rc == MIZU_ERR);
    assert(strstr(a.ch_msg, "cannot resolve liveness lock directory") !=
           NULL);
    assert(a.pl_rc == MIZU_ERR);
    assert(strstr(a.pl_msg, "cannot resolve liveness lock directory") !=
           NULL);
#endif

    memset(&a, 0, sizeof(a));
    a.tmpdir = NULL;
    assert(pthread_create(&th, NULL, live_dir_main, &a) == 0);
    assert(pthread_join(th, NULL) == 0);
    assert(a.dir != NULL);
    assert(a.sh_rc == MIZU_OK);

    if (tmp0 != NULL)
      setenv("TMPDIR", saved_tmp, 1);
    else
      unsetenv("TMPDIR");
  }

  /* a forked child with TMPDIR unset resolves the registry log dir via
     the platform default (confstr on macOS) — the resolution runs per
     process, so only a child re-opens its log */
  {
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
      unsetenv("TMPDIR");
      mizu_shm *s = NULL;
      if (mizu_shm_create(&s, 4096) != MIZU_OK) exit(1);
      mizu_shm_close(s, 1);
      exit(0);
    }
    int st;
    assert(waitpid(pid, &st, 0) == pid);
    assert(WIFEXITED(st) && WEXITSTATUS(st) == 0);
  }

  /* a signal to the parked thread breaks its sleep as PARK_INTR */
  {
    mizu_shm *ctl2 = NULL;
    assert(mizu_shm_create(&ctl2, 4096) == MIZU_OK);
    _Atomic uint32_t *epoch2 = (_Atomic uint32_t *) mizu_shm_addr(ctl2);
    assert(mizu_parker_attach(&intr_pk, epoch2, mizu_shm_name(ctl2),
                             MIZU_ENTITY_HOST, 1) == 0);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = eintr_handler;
    assert(sigaction(SIGUSR1, &sa, NULL) == 0);
    pthread_t th;
    assert(pthread_create(&th, NULL, eintr_park_main, NULL) == 0);
    struct timespec ts = { 0, 100000000 };   /* 100 ms: parked by now */
    nanosleep(&ts, NULL);
    assert(pthread_kill(th, SIGUSR1) == 0);
    void *rc = NULL;
    assert(pthread_join(th, &rc) == 0);
    assert((intptr_t) rc == MIZU_PARK_INTR);
    mizu_parker_detach(&intr_pk);
    mizu_shm_close(ctl2, 1);
  }
#endif

#ifndef _WIN32
  /* a forked child: the atfork handler closes the inherited lock fds
     (the registry in the child is empty, so a close of the stale handle
     copy is a no-op) and the pid cache resets. The child exit()s so its
     coverage profile flushes. */
  {
    char hpath2[1100];
    int hn = snprintf(hpath2, sizeof(hpath2), "%s/mizu_test_atfork_%ld.live",
                      dir, mizu_self_pid());
    assert(hn > 0 && (size_t) hn < sizeof(hpath2));
    intptr_t hf;
    assert(mizu_live_open(hpath2, &hf) == 0);
    long parent_pid = mizu_self_pid();
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
      mizu_live_close(hf);   /* already closed by the handler: no-op */
      if (mizu_self_pid() == parent_pid || mizu_self_pid() != (long) getpid())
        exit(1);
      if (mizu_live_dir() == NULL) exit(2);
      exit(0);
    }
    int st;
    assert(waitpid(pid, &st, 0) == pid);
    assert(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    mizu_live_close(hf);
    remove(hpath2);
  }
#endif

  puts("test_region: ok");
  return 0;
}
