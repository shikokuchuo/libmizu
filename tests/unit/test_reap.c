/* Reaping: orphan regions (shm.c), the worker-reap serialization
   (pool.c), and the owner-death orphan teardown. The orphan test: a
   child creates a region and exits without teardown, leaving the region
   — and on macOS its registry log — behind. The parent's mizu_shm_reap
   must report the name, unlink the region, and drop the dead process's
   log. The race test: the controller's kept-fd probe and its off-thread
   death-callback reap must never run concurrently on one slot. The
   owner-death test: an attached submitter's watch must fire on the
   owner's exit, and the first verb past the verdict must run the orphan
   teardown (files removed, dead creator's regions reaped). Exercises
   the Linux /dev/shm scan and the macOS log scan; other platforms have
   no enumerable namespace. Worker children exec this binary (argv mode
   "child") so a threaded parent never forks. Run via `make test`. */

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

static void test_orphan_region(void) {
  /* Scratch registry dir: the macOS log resolves $TMPDIR per call. */
  char scratch[PATH_MAX];
  int sn = snprintf(scratch, sizeof(scratch), "/tmp/mizu_test_reap_%ld",
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
    mizu_shm *shm = NULL;
    if (mizu_shm_create(&shm, 4096) == MIZU_OK) {
      size_t n = strlen(mizu_shm_name(shm)) + 1;
      ssize_t w = write(pipefd[1], mizu_shm_name(shm), n);
      (void) w;
    }
    _exit(0);                  /* no teardown: an orphan region (+ log) */
  }
  close(pipefd[1]);

  char name[MIZU_NAME_MAX];
  ssize_t got = read(pipefd[0], name, sizeof(name) - 1);
  close(pipefd[0]);
  assert(got > 0 && name[got - 1] == '\0');

  int st;
  assert(waitpid(pid, &st, 0) == pid);     /* no zombie: the pid reads dead */

#ifdef __APPLE__
  /* The child's log exists and names the orphan. */
  char logpath[PATH_MAX];
  int ln = snprintf(logpath, sizeof(logpath), "%s/mizu/mizu_%x",
                    scratch, (unsigned) pid);
  assert(ln > 0 && (size_t) ln < sizeof(logpath));
  assert(access(logpath, F_OK) == 0);
#endif

  int n = 0;
  char **reaped = mizu_shm_reap(&n);
  int found = 0;
  for (int i = 0; i < n; i++) {
    if (strcmp(reaped[i], name) == 0) found = 1;
    free(reaped[i]);
  }
  free(reaped);
  assert(found);

  /* The region is gone. */
  mizu_shm *gone = NULL;
  assert(mizu_shm_open(&gone, name) == MIZU_ERR);

#ifdef __APPLE__
  /* The dead process's log is gone too, and the registry dir pruned. */
  errno = 0;
  assert(access(logpath, F_OK) != 0 && errno == ENOENT);
  char regdir[PATH_MAX];
  int rn = snprintf(regdir, sizeof(regdir), "%s/mizu", scratch);
  assert(rn > 0 && (size_t) rn < sizeof(regdir));
  errno = 0;
  assert(access(regdir, F_OK) != 0 && errno == ENOENT);
#endif

  assert(rmdir(scratch) == 0);
  puts("ok orphan_region");
}

// The probe/callback race ------------------------------------------------------------

static int cb_strand;

static int cb_exec(const mizu_slot_hdr *hdr, const uint8_t *payload,
                   size_t limit, mizu_result_sink *sink, int catching,
                   mizu_read_ctx *ctx) {
  (void) limit;
  (void) catching;
  (void) ctx;
  if (cb_strand) return 0;             /* claim, never publish */
  assert(hdr->kind == MIZU_KIND_INLINE);
  mizu_bytes b = { (void *) payload, hdr->len };
  assert(mizu_result_publish(sink, &b) == 1);
  return 0;
}

/* The worker child (exec'd: joins, claims one task, exits without
   leave — the slot reads LIVE with the lock released, exactly a crash).
   _exit skips the atexit leak report (ASan's LSan would flag the
   intentionally leaked handle and mask the exit code). */
static void reap_race_child(const char *token, int strand) {
  cb_strand = strand;
  mizu_binding wb;
  mizu_binding_bytes(&wb);
  wb.exec = cb_exec;
  mizu_pool *cwk;
  if (mizu_pool_worker_join(&cwk, token, 0, &wb) != MIZU_OK) _exit(2);
  (void) mizu_pool_step(cwk, 5000);
  _exit(0);
}

static pid_t spawn_child(const char *self, const char *token, int strand) {
  char st[16];
  snprintf(st, sizeof(st), "%d", strand);
  pid_t pid = fork();
  assert(pid >= 0);
  if (pid == 0) {
    execl(self, self, "child", token, st, (char *) NULL);
    _exit(9);
  }
  return pid;
}

static void wait_child(pid_t pid) {
  int wst;
  assert(waitpid(pid, &wst, 0) == pid);
  assert(WIFEXITED(wst) && WEXITSTATUS(wst) == 0);
}

static void await_slot_free(mizu_pool *ctrl) {
  mizu_pool_status ps;
  int freed = 0;
  MIZU_SPIN_WAIT((mizu_pool_status_get(ctrl, &ps) == MIZU_OK &&
                  ps.worker_state[0] == MIZU_WK_FREE),
                 mizu_now() + 5, MIZU_SPIN_CLOCK_EVERY_LIGHT, freed);
  assert(freed);
}

/* A claimed worker dies while the handle thread probes from collect and
   the death callback reaps off-thread — one reaper per slot (the
   reap-owner word). The respawned worker's fresh task must survive: a
   double reaper could stamp its PENDING to DIED mid-sweep or fail the
   join "stale spawn". */
static void test_reap_probe_cb_race(const char *self) {
  mizu_pool_opts o;
  mizu_pool_opts_init(&o);
  o.max_workers = 1;
  o.max_submitters = 1;
  o.injection_cap = 64;
  o.per_worker_cap = 64;
  o.result_slots = 512;      /* a long reap sweep widens any overlap */
  o.slot_size = 256;
  mizu_binding b;
  mizu_binding_bytes(&b);
  mizu_pool *ctrl;
  assert(mizu_pool_create(&ctrl, &o, &b) == MIZU_OK);
  char token[64];
  assert(mizu_pool_token(ctrl, token, sizeof token) == MIZU_OK);

  for (int i = 0; i < 8; i++) {
    pid_t pid = spawn_child(self, token, 1);
    uint32_t s0 = 0;
    assert(mizu_pool_ready_wait(ctrl, &s0, 1, 5000) == MIZU_OK);
    char payload[16];
    int pn = snprintf(payload, sizeof(payload), "t%d", i);
    assert(pn > 0);
    mizu_bytes t1 = { payload, (size_t) pn };
    mizu_task t;
    assert(mizu_pool_submit(ctrl, &t1, &t, 5000) == MIZU_OK);
    wait_child(pid);

    /* the race window: the callback reaps while collect's backstop
       probes — the verdict must converge, exactly once */
    void *obj = NULL;
    mizu_status rc = MIZU_OK;
    for (int k = 0; k < 100 && rc != MIZU_ERR; k++)
      rc = mizu_pool_collect(ctrl, &t, &obj, 50);
    assert(rc == MIZU_ERR);
    assert(strcmp(mizu_pool_error(ctrl),
                  "worker died while executing this task") == 0);
    await_slot_free(ctrl);

    /* respawn: the fresh incarnation's work survives any straggler */
    pid = spawn_child(self, token, 0);
    assert(mizu_pool_ready_wait(ctrl, &s0, 1, 5000) == MIZU_OK);
    assert(mizu_pool_submit(ctrl, &t1, &t, 5000) == MIZU_OK);
    obj = NULL;
    assert(mizu_pool_collect(ctrl, &t, &obj, 5000) == MIZU_OK);
    mizu_bytes *r = obj;
    assert(r->len == t1.len && memcmp(r->data, t1.data, r->len) == 0);
    mizu_bytes_free(r);
    wait_child(pid);
    await_slot_free(ctrl);
  }
  assert(mizu_pool_stop(ctrl, 5000) == MIZU_OK);
  mizu_pool_destroy(ctrl);
  puts("ok reap_probe_cb_race");
}

#ifdef __linux__
/* Lock files are not regions: the /dev/shm scan must skip
   "mizu_<pid>_<ctr>.live.host", "....wk.0", etc. — reaping them under
   live workers would blind the sibling probes (ENOENT reads
   indeterminate, never a verdict). The unit tier creates no lock files
   of its own, so this plants both grammars under one dead pid. */
static void test_reap_skips_lock_files(void) {
  pid_t pid = fork();
  assert(pid >= 0);
  if (pid == 0) _exit(0);
  int st;
  assert(waitpid(pid, &st, 0) == pid);         /* a guaranteed-dead pid */

  char base[160];
  int bn = snprintf(base, sizeof(base), "/dev/shm/mizu_%lx_9",
                    (unsigned long) pid);
  assert(bn > 0 && (size_t) bn < sizeof(base));
  const char *suffixes[] = { ".live.host", ".live.peer", ".wk.0",
                             ".sub.1", ".owner" };
  for (size_t i = 0; i < sizeof suffixes / sizeof *suffixes; i++) {
    char path[192];
    int pn = snprintf(path, sizeof(path), "%s%s", base, suffixes[i]);
    assert(pn > 0 && (size_t) pn < sizeof(path));
    FILE *f = fopen(path, "we");
    assert(f != NULL && fclose(f) == 0);
  }
  /* the exact region grammar under the same dead pid: must reap —
     proves the scan ran rather than trivially matching nothing */
  FILE *f = fopen(base, "we");
  assert(f != NULL && fclose(f) == 0);

  int n = 0;
  char **reaped = mizu_shm_reap(&n);
  int found = 0;
  char want[MIZU_NAME_MAX];
  int wn = snprintf(want, sizeof(want), "/mizu_%lx_9", (unsigned long) pid);
  assert(wn > 0 && (size_t) wn < sizeof(want));
  for (int i = 0; i < n; i++) {
    if (strcmp(reaped[i], want) == 0) found = 1;
    free(reaped[i]);
  }
  free(reaped);
  assert(found);

  for (size_t i = 0; i < sizeof suffixes / sizeof *suffixes; i++) {
    char path[192];
    int pn = snprintf(path, sizeof(path), "%s%s", base, suffixes[i]);
    assert(pn > 0 && (size_t) pn < sizeof(path));
    assert(access(path, F_OK) == 0);           /* still there */
    assert(remove(path) == 0);
  }
  puts("ok reap_skips_lock_files");
}
#endif

// Owner death: the orphan teardown ---------------------------------------------

/* The pool-owning child (a plain fork, like the orphan-region child's):
   one create, the token across the pipe, then _exit on the parent's
   go-ahead. The parent attaches while the owner is alive (the owner
   watch arms on a foreign pid), the child's death fires it, and the
   first verb past the verdict runs the orphan teardown: the owner lock
   acquires, the shutdown broadcasts, the liveness files are removed,
   and the dead creator's regions are reaped. */
static void test_owner_death(void) {
  char scratch[PATH_MAX];
  int sn = snprintf(scratch, sizeof(scratch), "/tmp/mizu_test_owner_%ld",
                    (long) getpid());
  assert(sn > 0 && (size_t) sn < sizeof(scratch));
  assert(mkdir(scratch, 0700) == 0);
  assert(setenv("TMPDIR", scratch, 1) == 0);

  int tok[2], go[2];
  assert(pipe(tok) == 0 && pipe(go) == 0);

  pid_t pid = fork();
  assert(pid >= 0);
  if (pid == 0) {
    close(tok[0]);
    close(go[1]);
    mizu_pool_opts o;
    mizu_pool_opts_init(&o);
    o.max_workers = 1;
    o.max_submitters = 2;
    o.injection_cap = 64;
    o.per_worker_cap = 64;
    o.result_slots = 64;
    o.slot_size = 256;
    mizu_binding b;
    mizu_binding_bytes(&b);
    mizu_pool *c = NULL;
    if (mizu_pool_create(&c, &o, &b) == MIZU_OK) {
      char token[64];
      if (mizu_pool_token(c, token, sizeof token) == MIZU_OK) {
        ssize_t w = write(tok[1], token, strlen(token) + 1);
        (void) w;
      }
    }
    char gate;
    ssize_t r = read(go[0], &gate, 1);
    (void) r;
    _exit(0);                  /* death without teardown: an orphan pool */
  }
  close(tok[1]);
  close(go[0]);

  char token[64];
  ssize_t got = read(tok[0], token, sizeof(token) - 1);
  close(tok[0]);
  assert(got > 0 && token[got - 1] == '\0');

  mizu_binding b;
  mizu_binding_bytes(&b);
  mizu_pool *sub = NULL;
  assert(mizu_pool_attach(&sub, token, &b) == MIZU_OK);

  assert(write(go[1], "x", 1) == 1);
  close(go[1]);
  int st;
  assert(waitpid(pid, &st, 0) == pid);

  /* the death listener translates the owner's exit into owner_dead */
  mizu_pool_sig *sig = mizu_pool_signals(sub);
  assert(sig != NULL);
  int saw_dead = 0;
  MIZU_SPIN_WAIT(*sig->owner_dead == 1,
                 mizu_now() + 5, MIZU_SPIN_CLOCK_EVERY_LIGHT, saw_dead);
  assert(saw_dead);
  free(sig);

  /* the first verb past the verdict runs the orphan teardown */
  mizu_bytes t1 = { (void *) "x", 1 };
  mizu_task t;
  assert(mizu_pool_submit(sub, &t1, &t, 0) == MIZU_ERR);
  assert(strcmp(mizu_pool_error(sub), "pool stopped or owner dead") == 0);

  /* ... which removed the liveness files and reaped the dead creator's
     region */
  char opath[PATH_MAX];
  int on = snprintf(opath, sizeof(opath), "%s/mizu_%s.owner",
                    mizu_live_dir(), token);
  assert(on > 0 && (size_t) on < sizeof(opath));
  errno = 0;
  assert(access(opath, F_OK) != 0 && errno == ENOENT);
  char region[MIZU_NAME_MAX];
  int rn = snprintf(region, sizeof(region), "%s%s", MIZU_PREFIX_LITERAL,
                    token);
  assert(rn > 0 && (size_t) rn < sizeof(region));
  mizu_shm *gone = NULL;
  assert(mizu_shm_open(&gone, region) == MIZU_ERR);

  /* teardown stops the armed watch */
  mizu_pool_destroy(sub);
  assert(rmdir(scratch) == 0);
  puts("ok owner_death");
}

int main(int argc, char **argv) {
  if (argc == 4 && strcmp(argv[1], "child") == 0)
    reap_race_child(argv[2], atoi(argv[3]));   /* _exit()s, never returns */
  test_reap_probe_cb_race(argv[0]);   /* before the orphan test clutters
                                         and deletes its scratch TMPDIR */
  test_orphan_region();
  test_owner_death();
#ifdef __linux__
  test_reap_skips_lock_files();
#endif
  puts("test_reap: ok");
  return 0;
}

#else

int main(void) {
  puts("test_reap: ok (no enumerable SHM namespace)");
  return 0;
}

#endif
