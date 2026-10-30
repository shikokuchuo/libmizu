/* Unit tier: notify endpoints — the arm/disarm protocol, the
   edge-triggered signal, the watch, and the endpoint mechanics, exercised
   in-process as channel and pool pairs staging through the built-in bytes
   binding. Assert-based; run via `make test`. */

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
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif
#ifdef __linux__
#include <sys/resource.h>
#endif

#include "internal.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static mizu_binding bytesb;
static _Atomic int fires;

static void count_cb(void *ctx) {
  (void) ctx;
  atomic_fetch_add_explicit(&fires, 1, memory_order_relaxed);
}

static void tiny_sleep(void) {
#ifdef _WIN32
  Sleep(1);
#else
  struct timespec ts = {0, 1000000L};
  nanosleep(&ts, NULL);
#endif
}

/* Poll-bound the asynchronous fire (the listener thread's or dispatch
   queue's callback). */
static int wait_fires(int at_least, int ms) {
  for (int i = 0; i < ms; i++) {
    if (atomic_load_explicit(&fires, memory_order_acquire) >= at_least)
      return 1;
    tiny_sleep();
  }
  return 0;
}

static void settle(int ms) {
  for (int i = 0; i < ms; i++) tiny_sleep();
}

// Channel harness ---------------------------------------------------------------

static void chan_pair(mizu_channel **host, mizu_channel **peer,
                      uint32_t flags) {
  mizu_channel_opts opts;
  mizu_channel_opts_init(&opts);
  opts.capacity = 64;
  opts.slot_size = 512;
  opts.flags = flags;
  assert(mizu_channel_create(host, &opts, &bytesb) == MIZU_OK);
  char token[64];
  assert(mizu_channel_token(*host, token, sizeof(token)) == MIZU_OK);
  assert(mizu_channel_attach(peer, token, &bytesb) == MIZU_OK);
  assert(mizu_channel_ready_set(*peer) == MIZU_OK);
  assert(mizu_channel_ready_wait(*host, 5000) == MIZU_OK);
}

static void chan_end(mizu_channel *host, mizu_channel *peer) {
  mizu_channel_destroy(host);
  mizu_channel_destroy(peer);
}

static uint64_t chan_sends(mizu_channel *c) {
  mizu_channel_info info;
  assert(mizu_channel_info_get(c, &info) == MIZU_OK);
  return info.notify_sends;
}

static void recv_one(mizu_channel *c) {
  void *obj = NULL;
  assert(mizu_channel_recv(c, &obj, 5000) == MIZU_OK);
  mizu_bytes_free(obj);
}

// Channel scenarios ---------------------------------------------------------------

/* A burst while armed signals exactly once; the drain sees every entry;
   a re-arm signals exactly once more. */
static void test_channel_burst(void) {
  mizu_channel *host, *peer;
  chan_pair(&host, &peer, 0);
  fires = 0;
  int avail = -1;
  assert(mizu_channel_notify_arm(peer, 1, &avail) == MIZU_OK);
  assert(avail == 0);
  assert(mizu_channel_notify_watch(peer, count_cb, NULL) == MIZU_OK);

  enum { N = 20 };
  mizu_bytes b = { (void *) "x", 1 };
  for (int i = 0; i < N; i++)
    assert(mizu_channel_send(host, &b) == MIZU_OK);

  assert(wait_fires(1, 5000));
  settle(50);
  assert(fires == 1);                    /* edge-triggered: one per arm */
  assert(chan_sends(host) == 1);
  for (int i = 0; i < N; i++) recv_one(peer);

  assert(mizu_channel_notify_arm(peer, 1, &avail) == MIZU_OK);
  assert(avail == 0);                    /* drained above */
  for (int i = 0; i < N; i++)
    assert(mizu_channel_send(host, &b) == MIZU_OK);
  assert(wait_fires(2, 5000));
  settle(50);
  assert(fires == 2);
  assert(chan_sends(host) == 2);
  for (int i = 0; i < N; i++) recv_one(peer);

  assert(mizu_channel_notify_unwatch(peer) == MIZU_OK);
  assert(mizu_channel_notify_arm(peer, 0, NULL) == MIZU_OK);
  chan_end(host, peer);
  puts("ok channel_burst");
}

/* The arm reports what is already there (the synchronous drain resolves
   it); a signal queued while un-watched is dropped at the next arm, so a
   later-installed watch fires only for fresh data. */
static void test_channel_arm_reports(void) {
  mizu_channel *host, *peer;
  chan_pair(&host, &peer, 0);
  fires = 0;
  int avail = -1;

  /* arm (no watch yet) -> send: one datagram queued, nobody drains it */
  assert(mizu_channel_notify_arm(peer, 1, &avail) == MIZU_OK);
  assert(avail == 0);
  mizu_bytes b = { (void *) "y", 1 };
  assert(mizu_channel_send(host, &b) == MIZU_OK);
  assert(chan_sends(host) == 1);
  assert(mizu_channel_notify_arm(peer, 0, NULL) == MIZU_OK);

  /* re-arm: the stale datagram is dropped; availability reports the
     message still in the ring */
  assert(mizu_channel_notify_arm(peer, 1, &avail) == MIZU_OK);
  assert(avail == 1);
  recv_one(peer);

  /* the watch installed now must not fire for the stale signal */
  assert(mizu_channel_notify_watch(peer, count_cb, NULL) == MIZU_OK);
  settle(150);
  assert(fires == 0);

  /* fresh data fires exactly once */
  assert(mizu_channel_send(host, &b) == MIZU_OK);
  assert(wait_fires(1, 5000));
  settle(50);
  assert(fires == 1);
  assert(chan_sends(host) == 2);
  recv_one(peer);

  /* disarm: publishes no longer signal */
  assert(mizu_channel_notify_arm(peer, 0, NULL) == MIZU_OK);
  assert(mizu_channel_send(host, &b) == MIZU_OK);
  settle(150);
  assert(fires == 1);
  assert(chan_sends(host) == 2);
  recv_one(peer);

  assert(mizu_channel_notify_unwatch(peer) == MIZU_OK);
  chan_end(host, peer);
  puts("ok channel_arm_reports");
}

/* A spin channel has no notify endpoint (its publish path skips the wake
   checkpoint): arm and watch fail on both sides. */
static void test_channel_spin(void) {
  mizu_channel *host, *peer;
  chan_pair(&host, &peer, MIZU_FLAG_SPIN);
  assert(mizu_channel_notify_arm(host, 1, NULL) == MIZU_ERR);
  assert(mizu_channel_notify_arm(peer, 1, NULL) == MIZU_ERR);
  assert(mizu_channel_notify_watch(host, count_cb, NULL) == MIZU_ERR);
  assert(mizu_channel_notify_watch(peer, count_cb, NULL) == MIZU_ERR);
  /* the error is recorded on the handle */
  assert(mizu_channel_errcat(peer) == MIZU_ERRCAT_OTHER);
  chan_end(host, peer);
  puts("ok channel_spin");
}

/* The close path signals the endpoint alongside the parker unpark: an
   armed consumer's drain surfaces CLOSED. */
static void test_channel_close_wakes(void) {
  mizu_channel *host, *peer;
  chan_pair(&host, &peer, 0);
  fires = 0;
  assert(mizu_channel_notify_arm(peer, 1, NULL) == MIZU_OK);
  assert(mizu_channel_notify_watch(peer, count_cb, NULL) == MIZU_OK);
  assert(mizu_channel_close_signal(host) == MIZU_OK);
  assert(wait_fires(1, 5000));
  void *obj = NULL;
  assert(mizu_channel_recv(peer, &obj, 5000) == MIZU_CLOSED);
  assert(mizu_channel_notify_unwatch(peer) == MIZU_OK);
  chan_end(host, peer);
  puts("ok channel_close_wakes");
}

/* Destroy with a live watch stops it (no unwatch first). */
static void test_channel_destroy_watched(void) {
  mizu_channel *host, *peer;
  chan_pair(&host, &peer, 0);
  fires = 0;
  assert(mizu_channel_notify_arm(peer, 1, NULL) == MIZU_OK);
  assert(mizu_channel_notify_watch(peer, count_cb, NULL) == MIZU_OK);
  mizu_bytes b = { (void *) "z", 1 };
  assert(mizu_channel_send(host, &b) == MIZU_OK);
  assert(wait_fires(1, 5000));
  recv_one(peer);
  chan_end(host, peer);   /* destroy runs the notify teardown */
  puts("ok channel_destroy_watched");
}

// Pool harness ---------------------------------------------------------------------

static mizu_pool *ctrl, *wk;
static char token[64];

static int test_exec(const mizu_slot_hdr *hdr, const uint8_t *payload,
                     size_t limit, mizu_result_sink *sink, int catching,
                     mizu_read_ctx *ctx) {
  (void) catching;
  (void) ctx;
  assert(hdr->kind == MIZU_KIND_INLINE && hdr->len <= limit);
  mizu_bytes b = { (void *) payload, hdr->len };
  assert(mizu_result_publish(sink, &b) == 1);
  return 0;
}

static void *test_read(const mizu_slot_hdr *hdr, const uint8_t *payload,
                       size_t limit, mizu_read_ctx *ctx) {
  return bytesb.read(hdr, payload, limit, ctx);
}

static void pool_pair(void) {
  mizu_pool_opts opts;
  mizu_pool_opts_init(&opts);
  opts.max_workers = 1;
  opts.max_submitters = 8;
  opts.injection_cap = 64;
  opts.per_worker_cap = 64;
  opts.result_slots = 64;
  opts.slot_size = 512;
  mizu_binding b, wb;
  mizu_binding_bytes(&b);
  b.read = test_read;
  mizu_binding_bytes(&wb);
  wb.read = test_read;
  wb.exec = test_exec;
  assert(mizu_pool_create(&ctrl, &opts, &b) == MIZU_OK);
  assert(mizu_pool_token(ctrl, token, sizeof(token)) == MIZU_OK);
  assert(mizu_pool_worker_join(&wk, token, 0, &wb) == MIZU_OK);
}

static void pool_end(void) {
  assert(mizu_pool_leave(wk) == MIZU_OK);
  mizu_pool_destroy(wk);
  mizu_pool_destroy(ctrl);
}

// Pool scenarios ---------------------------------------------------------------------

/* K armed tasks complete: one signal for the burst, the dump's counter
   and waiter rows read the masked form, every task collectible. */
static void test_pool_burst(void) {
  pool_pair();
  fires = 0;
  enum { K = 6 };
  mizu_task tasks[K];
  mizu_bytes b = { (void *) "t", 1 };
  for (int i = 0; i < K; i++)
    assert(mizu_pool_submit(ctrl, &b, &tasks[i], 1000) == MIZU_OK);

  /* announce -> arm -> watch, the protocol order */
  assert(mizu_pool_task_notify(ctrl, tasks, K, 1) == MIZU_OK);
  assert(mizu_pool_notify_arm(ctrl, 1) == MIZU_OK);
  assert(mizu_pool_notify_watch(ctrl, count_cb, NULL) == MIZU_OK);

  /* the dump masks the NOTIFY bit: the row reads the plain slot */
  mizu_rs_row rows[64];
  uint32_t nr = 0;
  assert(mizu_pool_tasks_get(ctrl, rows, 64, &nr) == MIZU_OK);
  uint32_t announced = 0;
  for (uint32_t i = 0; i < nr; i++)
    if (rows[i].waiter_slot >= 0) {
      announced++;
      assert(rows[i].waiter_slot >= 0 && rows[i].waiter_slot < 8);
      assert((rows[i].waiter_slot & (1 << 30)) == 0);
    }
  assert(announced == K);

  for (int i = 0; i < K; i++)
    assert(mizu_pool_step(wk, 1000) == MIZU_STEP_TASK);

  assert(wait_fires(1, 5000));
  settle(50);
  assert(fires == 1);
  mizu_pool_dump dump;
  assert(mizu_pool_dump_get(wk, &dump) == MIZU_OK);
  assert(dump.notify_sends == 1);

  /* every task already terminal: the synchronous scan collects */
  for (int i = 0; i < K; i++) {
    void *obj = NULL;
    assert(mizu_pool_collect(ctrl, &tasks[i], &obj, 1000) == MIZU_OK);
    mizu_bytes_free(obj);
  }

  /* a plain blocking wait afterwards rides the unchanged announce regime */
  mizu_task t;
  assert(mizu_pool_submit(ctrl, &b, &t, 1000) == MIZU_OK);
  assert(mizu_pool_step(wk, 1000) == MIZU_STEP_TASK);
  void *obj = NULL;
  assert(mizu_pool_collect(ctrl, &t, &obj, 1000) == MIZU_OK);
  mizu_bytes_free(obj);

  assert(mizu_pool_notify_unwatch(ctrl) == MIZU_OK);
  assert(mizu_pool_notify_arm(ctrl, 0) == MIZU_OK);
  pool_end();
  puts("ok pool_burst");
}

/* A withdrawn announce wakes nobody: no signal, and the blocking collect
   afterwards is unaffected. */
static void test_pool_withdraw(void) {
  pool_pair();
  fires = 0;
  mizu_task tasks[2];
  mizu_bytes b = { (void *) "u", 1 };
  for (int i = 0; i < 2; i++)
    assert(mizu_pool_submit(ctrl, &b, &tasks[i], 1000) == MIZU_OK);
  assert(mizu_pool_task_notify(ctrl, tasks, 2, 1) == MIZU_OK);
  assert(mizu_pool_notify_arm(ctrl, 1) == MIZU_OK);
  assert(mizu_pool_notify_watch(ctrl, count_cb, NULL) == MIZU_OK);
  assert(mizu_pool_task_notify(ctrl, tasks, 2, 0) == MIZU_OK);
  assert(mizu_pool_notify_arm(ctrl, 0) == MIZU_OK);

  for (int i = 0; i < 2; i++)
    assert(mizu_pool_step(wk, 1000) == MIZU_STEP_TASK);
  settle(150);
  assert(fires == 0);
  mizu_pool_dump dump;
  assert(mizu_pool_dump_get(wk, &dump) == MIZU_OK);
  assert(dump.notify_sends == 0);

  for (int i = 0; i < 2; i++) {
    void *obj = NULL;
    assert(mizu_pool_collect(ctrl, &tasks[i], &obj, 1000) == MIZU_OK);
    mizu_bytes_free(obj);
  }
  assert(mizu_pool_notify_unwatch(ctrl) == MIZU_OK);
  pool_end();
  puts("ok pool_withdraw");
}

/* The verbs' gates: a plain worker handle (no submitter slot) cannot arm,
   watch, or announce; a malformed or stale task handle cannot announce. */
static void test_pool_gates(void) {
  pool_pair();
  assert(mizu_pool_notify_arm(wk, 1) == MIZU_ERR);
  assert(mizu_pool_notify_watch(wk, count_cb, NULL) == MIZU_ERR);
  mizu_task t;
  mizu_bytes b = { (void *) "v", 1 };
  assert(mizu_pool_submit(ctrl, &b, &t, 1000) == MIZU_OK);
  assert(mizu_pool_task_notify(wk, &t, 1, 1) == MIZU_ERR);
  mizu_task bad = mizu_task_make(1, 9999);
  assert(mizu_pool_task_notify(ctrl, &bad, 1, 1) == MIZU_ERR);
  /* re-sequence the slot (8 per submitter here): the stale handle's
     announce then refuses */
  assert(mizu_pool_step(wk, 1000) == MIZU_STEP_TASK);
  void *obj = NULL;
  assert(mizu_pool_collect(ctrl, &t, &obj, 1000) == MIZU_OK);
  mizu_bytes_free(obj);
  mizu_task w;
  for (int i = 0; i < 8; i++) {
    assert(mizu_pool_submit(ctrl, &b, &w, 1000) == MIZU_OK);
    assert(mizu_pool_step(wk, 1000) == MIZU_STEP_TASK);
    assert(mizu_pool_collect(ctrl, &w, &obj, 1000) == MIZU_OK);
    mizu_bytes_free(obj);
  }
  assert(mizu_pool_task_notify(ctrl, &t, 1, 1) == MIZU_ERR);
  pool_end();
  puts("ok pool_gates");
}

#ifndef _WIN32
/* A foreign owner's death is chained onto the notify watch: the armed
   submitter's callback fires with owner_dead set (an in-process owner
   stops itself — its broadcast signals endpoints). The forked-child
   abandon and the unwatch/release of the chained watch ride along. */
static void test_pool_owner_death_watch(void) {
  mizu_pool_opts opts;
  mizu_pool_opts_init(&opts);
  opts.max_workers = 1;
  opts.max_submitters = 2;
  opts.injection_cap = 8;
  opts.per_worker_cap = 8;
  opts.result_slots = 16;
  opts.slot_size = 256;
  int pipefd[2];
  assert(pipe(pipefd) == 0);
  pid_t pid = fork();
  assert(pid >= 0);
  if (pid == 0) {
    close(pipefd[0]);
    mizu_binding b;
    mizu_binding_bytes(&b);
    mizu_pool *dp = NULL;
    if (mizu_pool_create(&dp, &opts, &b) != MIZU_OK) _exit(2);
    char dtok[64];
    if (mizu_pool_token(dp, dtok, sizeof(dtok)) != MIZU_OK) _exit(3);
    size_t want = strlen(dtok) + 1;
    if (write(pipefd[1], dtok, want) != (ssize_t) want) _exit(4);
    sleep(30);                       /* the foreign owner, alive for now */
    _exit(0);
  }
  close(pipefd[1]);
  char dtok[64];
  size_t got = 0;
  while (got < sizeof(dtok)) {
    ssize_t r = read(pipefd[0], dtok + got, sizeof(dtok) - got);
    assert(r > 0);
    got += (size_t) r;
    if (memchr(dtok, '\0', got) != NULL) break;
  }
  close(pipefd[0]);

  mizu_pool *sub2 = NULL;
  assert(mizu_pool_attach(&sub2, dtok, &bytesb) == MIZU_OK);
  fires = 0;
  assert(mizu_pool_notify_arm(sub2, 1) == MIZU_OK);
  assert(mizu_pool_notify_watch(sub2, count_cb, NULL) == MIZU_OK);

  /* the forked child abandons its inherited armed handle: only the
     child's copies go, the parent's watch stays (plain exit: the
     atexit coverage flush rides along) */
  pid_t g = fork();
  assert(g >= 0);
  if (g == 0) {
    mizu_pool_abandon(sub2);
    exit(0);
  }
  int gst;
  assert(waitpid(g, &gst, 0) == g);
  assert(WIFEXITED(gst) && WEXITSTATUS(gst) == 0);

  /* the owner's death fires the chained watch: the callback runs and
     the drain reads the verdict */
  kill(pid, SIGKILL);
  int wst;
  assert(waitpid(pid, &wst, 0) == pid);
  assert(wait_fires(1, 5000));
  mizu_pool_sig *sig = mizu_pool_signals(sub2);
  assert(*sig->owner_dead == 1);
  free(sig);

  assert(mizu_pool_notify_unwatch(sub2) == MIZU_OK);
  /* re-watching a dead owner fires immediately */
  assert(mizu_pool_notify_watch(sub2, count_cb, NULL) == MIZU_OK);
  assert(wait_fires(2, 5000));

  mizu_pool_destroy(sub2);

  /* the orphan's names: the region through the platform reap, the
     liveness files by name */
  char name[64];
  int nl = snprintf(name, sizeof(name), MIZU_PREFIX_LITERAL "%s", dtok);
  assert(nl > 0 && (size_t) nl < sizeof(name));
  int n = 0;
  char **reaped = mizu_shm_reap(&n);
  int found = 0;
  for (int i = 0; i < n; i++) {
    if (strcmp(reaped[i], name) == 0) found = 1;
    free(reaped[i]);
  }
  free(reaped);
  assert(found);
  const char *dir = mizu_live_dir();
  char lp[1100];
  snprintf(lp, sizeof(lp), "%s/mizu_%s.owner", dir, dtok);
  remove(lp);
  snprintf(lp, sizeof(lp), "%s/mizu_%s.wk.0", dir, dtok);
  remove(lp);
  snprintf(lp, sizeof(lp), "%s/mizu_%s.sub.0", dir, dtok);
  remove(lp);
  snprintf(lp, sizeof(lp), "%s/mizu_%s.sub.1", dir, dtok);
  remove(lp);
  puts("ok pool_owner_death_watch");
}
#endif

#ifdef __linux__
/* RLIMIT_NOFILE caps the fd VALUE, not the count of open fds (mirrors
   the channel tier's helper): exactly k coexisting new fds is the soft
   limit one past the k-th free value. */
static rlim_t rlim_for_free(int k) {
  int free_n = 0;
  for (int i = 0; i < 65536; i++) {
    if (fcntl(i, F_GETFD) != -1) continue;
    if (k == 0) return (rlim_t) i;
    if (free_n + 1 == k) return (rlim_t) i + 1;
    free_n++;
  }
  return (rlim_t) -1;
}

/* The owner death watch failing to start rolls the notify watch back:
   fd exhaustion fails the pidfd open after the endpoint and its watch
   succeeded; the verb reports the failure and the handle recovers. */
static void test_pool_owner_watch_fail(void) {
  mizu_pool_opts opts;
  mizu_pool_opts_init(&opts);
  opts.max_workers = 1;
  opts.max_submitters = 2;
  opts.injection_cap = 8;
  opts.per_worker_cap = 8;
  opts.result_slots = 16;
  opts.slot_size = 256;
  int pipefd[2];
  assert(pipe(pipefd) == 0);
  pid_t pid = fork();
  assert(pid >= 0);
  if (pid == 0) {
    close(pipefd[0]);
    mizu_binding b;
    mizu_binding_bytes(&b);
    mizu_pool *dp = NULL;
    if (mizu_pool_create(&dp, &opts, &b) != MIZU_OK) _exit(2);
    char dtok[64];
    if (mizu_pool_token(dp, dtok, sizeof(dtok)) != MIZU_OK) _exit(3);
    size_t want = strlen(dtok) + 1;
    if (write(pipefd[1], dtok, want) != (ssize_t) want) _exit(4);
    sleep(30);                       /* the foreign owner, alive for now */
    _exit(0);
  }
  close(pipefd[1]);
  char dtok[64];
  size_t got = 0;
  while (got < sizeof(dtok)) {
    ssize_t r = read(pipefd[0], dtok + got, sizeof(dtok) - got);
    assert(r > 0);
    got += (size_t) r;
    if (memchr(dtok, '\0', got) != NULL) break;
  }
  close(pipefd[0]);

  mizu_pool *sub2 = NULL;
  assert(mizu_pool_attach(&sub2, dtok, &bytesb) == MIZU_OK);

  /* exactly two free fds: the endpoint socket and its dup succeed, the
     owner watch's pidfd fails */
  struct rlimit rl;
  assert(getrlimit(RLIMIT_NOFILE, &rl) == 0);
  rlim_t saved = rl.rlim_cur;
  rl.rlim_cur = rlim_for_free(2);
  assert(setrlimit(RLIMIT_NOFILE, &rl) == 0);
  assert(mizu_pool_notify_watch(sub2, count_cb, NULL) == MIZU_ERR);
  assert(strstr(mizu_pool_error(sub2), "cannot watch owner process") !=
         NULL);
  rl.rlim_cur = saved;
  assert(setrlimit(RLIMIT_NOFILE, &rl) == 0);

  /* the rollback left nothing half-armed: a fresh watch succeeds */
  assert(mizu_pool_notify_watch(sub2, count_cb, NULL) == MIZU_OK);
  assert(mizu_pool_notify_unwatch(sub2) == MIZU_OK);

  kill(pid, SIGKILL);
  int wst;
  assert(waitpid(pid, &wst, 0) == pid);
  mizu_pool_destroy(sub2);

  /* the orphan's names, as in the other foreign-owner scenario */
  char name[64];
  int nl = snprintf(name, sizeof(name), MIZU_PREFIX_LITERAL "%s", dtok);
  assert(nl > 0 && (size_t) nl < sizeof(name));
  int n = 0;
  char **reaped = mizu_shm_reap(&n);
  int found = 0;
  for (int i = 0; i < n; i++) {
    if (strcmp(reaped[i], name) == 0) found = 1;
    free(reaped[i]);
  }
  free(reaped);
  assert(found);
  const char *dir = mizu_live_dir();
  char lp[1100];
  snprintf(lp, sizeof(lp), "%s/mizu_%s.owner", dir, dtok);
  remove(lp);
  snprintf(lp, sizeof(lp), "%s/mizu_%s.wk.0", dir, dtok);
  remove(lp);
  snprintf(lp, sizeof(lp), "%s/mizu_%s.sub.0", dir, dtok);
  remove(lp);
  snprintf(lp, sizeof(lp), "%s/mizu_%s.sub.1", dir, dtok);
  remove(lp);
  puts("ok pool_owner_watch_fail");
}
#endif

// Endpoint mechanics -------------------------------------------------------------------

/* Direct endpoint/tx behavior: the stale-path absorb, the filesystem
   residue discipline, and the sun_path budget. */
static void test_endpoint_paths(void) {
  const char *livedir = mizu_live_dir();
  assert(livedir != NULL);
  const char *err = NULL;
  const char *region =
#ifdef _WIN32
    "Local\\mizu_1_2";
#else
    "/mizu_1_2";
#endif

  mizu_notify_ep *ep1 = mizu_notify_ep_open(livedir, region, "host", &err);
  assert(ep1 != NULL);
  /* a dead holder's residue (a close without the unlink) never blocks a
     fresh endpoint: unlink-before-bind absorbs it */
  mizu_notify_ep_close(ep1, 0);
  mizu_notify_ep *ep2 = mizu_notify_ep_open(livedir, region, "host", &err);
  assert(ep2 != NULL);

  /* a send reaches the endpoint and the drain empties it */
  mizu_notify_tx *tx = mizu_notify_tx_open(livedir, region, "host");
  assert(tx != NULL);
  mizu_notify_tx_send(tx);
  mizu_notify_ep_drain(ep2);
  mizu_notify_tx_close(tx);
  mizu_notify_ep_close(ep2, 1);

  char path[MIZU_PATH_BUF];
  int n = snprintf(path, sizeof(path), "%s/mizu_1_2.ntfy.host", livedir);
  assert(n > 0 && (size_t) n < sizeof(path));
#ifndef _WIN32
  /* the filesystem socket is unlinked by the graceful close */
  assert(access(path, F_OK) != 0);
  /* a stale path of any kind never blocks a fresh endpoint */
  int fd = open(path, O_CREAT | O_WRONLY, 0600);
  assert(fd >= 0);
  close(fd);
  mizu_notify_ep *ep4 = mizu_notify_ep_open(livedir, region, "host", &err);
  assert(ep4 != NULL);
  mizu_notify_ep_close(ep4, 1);
#endif

  /* the sun_path budget: POSIX records the failure; the named event
     (Windows) ignores livedir */
  char longdir[160];
  memset(longdir, 'a', sizeof(longdir) - 1);
  longdir[0] = '/';
  longdir[sizeof(longdir) - 1] = '\0';
  mizu_notify_ep *ep3 = mizu_notify_ep_open(longdir, region, "host", &err);
#ifndef _WIN32
  assert(ep3 == NULL);
  assert(err != NULL && strstr(err, "livedir too long") != NULL);
#else
  assert(ep3 != NULL);
  mizu_notify_ep_close(ep3, 0);
#endif
  (void) path;
  puts("ok endpoint_paths");
}

/* The reaper's exact region grammar leaves .ntfy names untouched. */
static void test_reap_grammar(void) {
#ifdef _WIN32
  puts("ok reap_grammar (n/a: Windows cannot enumerate the shm namespace)");
#else
  const char *livedir = mizu_live_dir();
  assert(livedir != NULL);
  char path[MIZU_PATH_BUF];
  int n = snprintf(path, sizeof(path), "%s/mizu_1_2.ntfy.host", livedir);
  assert(n > 0 && (size_t) n < sizeof(path));
  int fd = open(path, O_CREAT | O_WRONLY, 0600);
  assert(fd >= 0);
  close(fd);
  int count = 0;
  char **names = mizu_shm_reap(&count);
  int found = 0;
  for (int i = 0; i < count; i++) {
    if (strstr(names[i], ".ntfy.") != NULL) found = 1;
    free(names[i]);
  }
  free(names);
  assert(!found);
  /* and the file itself survives (never unlinked as an orphan) */
  assert(access(path, F_OK) == 0);
  assert(unlink(path) == 0);
  puts("ok reap_grammar");
#endif
}

int main(void) {
  mizu_binding_bytes(&bytesb);
  test_channel_burst();
  test_channel_arm_reports();
  test_channel_spin();
  test_channel_close_wakes();
  test_channel_destroy_watched();
  test_pool_burst();
  test_pool_withdraw();
  test_pool_gates();
#ifndef _WIN32
  test_pool_owner_death_watch();
#endif
#ifdef __linux__
  test_pool_owner_watch_fail();
#endif
  test_endpoint_paths();
  test_reap_grammar();
  puts("test_notify: all passed");
  return 0;
}
