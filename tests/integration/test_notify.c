/* Integration tier: the notify endpoint across real processes — a child
   arms and watches, the parent's burst signals exactly once, the
   peer-death chain fires the callback (a terminal that publishes nothing
   still wakes the drain), and a pool worker's publishes reach the armed
   submitter. Compiles against the installed headers (mizu.h + mizu_ext.h)
   with internal.h absent: the API's compile-time contract check.
   POSIX (fork); a stub passes elsewhere. Run via `make test-integration`. */

/* _GNU_SOURCE: glibc hides mkdtemp/setenv and other POSIX declarations
   under strict -std=c11. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE
#endif

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mizu.h"
#include "mizu_ext.h"

#ifdef _WIN32

int main(void) {
  puts("test_notify integration: skipped on Windows (fork-based)");
  return 0;
}

#else

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

static char token[64];
static _Atomic int fires;

static void count_cb(void *ctx) {
  (void) ctx;
  atomic_fetch_add_explicit(&fires, 1, memory_order_relaxed);
}

static void tiny_sleep(void) {
  struct timespec ts = {0, 1000000L};
  nanosleep(&ts, NULL);
}

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

static int wait_exit(pid_t pid) {
  int st = 0;
  assert(waitpid(pid, &st, 0) == pid);
  return WIFEXITED(st) ? WEXITSTATUS(st) : 128;
}

/* The armed peer: arm and watch first (the parent waits for the go
   byte), then the burst wakes exactly once and every entry drains. */
static void run_armed_peer(int n, int go_wr) {
  mizu_binding b;
  mizu_binding_bytes(&b);
  mizu_channel *p;
  if (mizu_channel_attach(&p, token, &b) != MIZU_OK) _exit(3);
  if (mizu_channel_ready_set(p) != MIZU_OK) _exit(4);
  if (mizu_channel_notify_arm(p, 1, NULL) != MIZU_OK) _exit(5);
  if (mizu_channel_notify_watch(p, count_cb, NULL) != MIZU_OK) _exit(6);
  char go = 'a';
  if (write(go_wr, &go, 1) != 1) _exit(7);
  close(go_wr);
  if (!wait_fires(1, 5000)) _exit(8);
  settle(50);
  if (fires != 1) _exit(9);            /* edge-triggered: one per arm */
  for (int i = 0; i < n; i++) {
    void *obj = NULL;
    if (mizu_channel_recv(p, &obj, 5000) != MIZU_OK) _exit(10);
    mizu_bytes_free(obj);
  }
  void *obj = NULL;
  if (mizu_channel_recv(p, &obj, 0) != MIZU_TIMEOUT) _exit(11);
  if (mizu_channel_notify_unwatch(p) != MIZU_OK) _exit(12);
  if (mizu_channel_notify_arm(p, 0, NULL) != MIZU_OK) _exit(13);
  mizu_channel_destroy(p);
  _exit(0);
}

static void test_cross_process_burst(void) {
  mizu_binding b;
  mizu_binding_bytes(&b);
  mizu_channel_opts opts;
  mizu_channel_opts_init(&opts);
  opts.capacity = 64;
  opts.slot_size = 512;
  mizu_channel *host;
  assert(mizu_channel_create(&host, &opts, &b) == MIZU_OK);
  assert(mizu_channel_token(host, token, sizeof(token)) == MIZU_OK);

  enum { N = 20 };
  int go[2];
  assert(pipe(go) == 0);
  fflush(stdout);
  pid_t p = fork();
  assert(p >= 0);
  if (p == 0) {
    close(go[0]);
    run_armed_peer(N, go[1]);
  }
  close(go[1]);
  assert(mizu_channel_ready_wait(host, 5000) == MIZU_OK);
  /* the peer is armed before anything is sent */
  char c;
  assert(read(go[0], &c, 1) == 1);
  close(go[0]);
  /* one flush for the whole burst: one signal, deterministically */
  mizu_bytes msg = { (void *) "ping", 4 };
  void *objs[N];
  for (int i = 0; i < N; i++) objs[i] = &msg;
  size_t accepted = 0;
  assert(mizu_channel_send_batch(host, objs, N, &accepted) == MIZU_OK);
  assert(accepted == N);

  assert(wait_exit(p) == 0);
  mizu_channel_info info;
  assert(mizu_channel_info_get(host, &info) == MIZU_OK);
  assert(info.notify_sends == 1);
  mizu_channel_destroy(host);
  puts("ok cross_process_burst");
}

/* The channel host child: creates, hands the token over, then dies hard
   once the peer is attached — the peer's armed drain must still wake. */
static void run_dying_host(int token_wr, int go_rd) {
  mizu_binding b;
  mizu_binding_bytes(&b);
  mizu_channel_opts opts;
  mizu_channel_opts_init(&opts);
  opts.capacity = 64;
  opts.slot_size = 512;
  mizu_channel *h;
  if (mizu_channel_create(&h, &opts, &b) != MIZU_OK) _exit(3);
  char tok[64];
  if (mizu_channel_token(h, tok, sizeof(tok)) != MIZU_OK) _exit(4);
  if (write(token_wr, tok, strlen(tok) + 1) != (ssize_t) (strlen(tok) + 1))
    _exit(5);
  char go;
  if (read(go_rd, &go, 1) != 1) _exit(6);
  _exit(0);   /* hard: no close signal, no destroy */
}

static void test_death_chain(void) {
  int tok_pipe[2], go_pipe[2];
  assert(pipe(tok_pipe) == 0 && pipe(go_pipe) == 0);
  fflush(stdout);
  pid_t h = fork();
  assert(h >= 0);
  if (h == 0) {
    close(tok_pipe[0]);
    close(go_pipe[1]);
    run_dying_host(tok_pipe[1], go_pipe[0]);
  }
  close(tok_pipe[1]);
  close(go_pipe[0]);
  assert(read(tok_pipe[0], token, sizeof(token)) > 0);
  close(tok_pipe[0]);

  mizu_binding b;
  mizu_binding_bytes(&b);
  mizu_channel *peer;
  assert(mizu_channel_attach(&peer, token, &b) == MIZU_OK);
  assert(mizu_channel_ready_set(peer) == MIZU_OK);
  fires = 0;
  assert(mizu_channel_notify_arm(peer, 1, NULL) == MIZU_OK);
  assert(mizu_channel_notify_watch(peer, count_cb, NULL) == MIZU_OK);

  /* the host dies: the chained death watch fires the same callback, and
     the drain surfaces the verdict */
  char go = 'x';
  assert(write(go_pipe[1], &go, 1) == 1);
  close(go_pipe[1]);
  assert(wait_exit(h) == 0);
  assert(wait_fires(1, 5000));
  void *obj = NULL;
  assert(mizu_channel_recv(peer, &obj, 5000) == MIZU_PEER_GONE);
  assert(mizu_channel_notify_unwatch(peer) == MIZU_OK);
  mizu_channel_destroy(peer);
  puts("ok death_chain");
}

static int child_exec(const mizu_slot_hdr *hdr, const uint8_t *payload,
                      size_t limit, mizu_result_sink *sink, int catching,
                      mizu_read_ctx *ctx) {
  (void) catching;
  (void) ctx;
  if (hdr->kind != MIZU_KIND_INLINE || hdr->len > limit) return 1;
  mizu_bytes b = { (void *) payload, hdr->len };
  return mizu_result_publish(sink, &b) == 1 ? 0 : 1;
}

/* The worker child: joins after the submitter armed, runs until the
   pool stops. */
static void run_pool_worker(void) {
  mizu_binding b;
  mizu_binding_bytes(&b);
  b.exec = child_exec;
  mizu_pool *p;
  if (mizu_pool_worker_join(&p, token, 0, &b) != MIZU_OK) _exit(3);
  if (mizu_pool_worker_run(p) != MIZU_EXIT_SHUTDOWN) _exit(4);
  if (mizu_pool_leave(p) != MIZU_OK) _exit(5);
  mizu_pool_destroy(p);
  _exit(0);
}

static void test_pool_cross_process(void) {
  mizu_binding b;
  mizu_binding_bytes(&b);
  mizu_pool_opts opts;
  mizu_pool_opts_init(&opts);
  opts.max_workers = 1;
  opts.max_submitters = 8;
  opts.injection_cap = 64;
  opts.per_worker_cap = 64;
  opts.result_slots = 64;
  opts.slot_size = 512;
  mizu_pool *ctrl;
  assert(mizu_pool_create(&ctrl, &opts, &b) == MIZU_OK);
  assert(mizu_pool_token(ctrl, token, sizeof(token)) == MIZU_OK);

  enum { K = 6 };
  mizu_task tasks[K];
  mizu_bytes msg = { (void *) "t", 1 };
  for (int i = 0; i < K; i++)
    assert(mizu_pool_submit(ctrl, &msg, &tasks[i], 1000) == MIZU_OK);
  fires = 0;
  assert(mizu_pool_task_notify(ctrl, tasks, K, 1) == MIZU_OK);
  assert(mizu_pool_notify_arm(ctrl, 1) == MIZU_OK);
  assert(mizu_pool_notify_watch(ctrl, count_cb, NULL) == MIZU_OK);

  fflush(stdout);
  pid_t w = fork();
  assert(w >= 0);
  if (w == 0) run_pool_worker();

  /* the burst of K publishes reaches the armed submitter as one signal */
  assert(wait_fires(1, 5000));
  settle(50);
  assert(fires == 1);
  for (int i = 0; i < K; i++) {
    void *obj = NULL;
    assert(mizu_pool_collect(ctrl, &tasks[i], &obj, 5000) == MIZU_OK);
    mizu_bytes_free(obj);
  }

  assert(mizu_pool_notify_unwatch(ctrl) == MIZU_OK);
  assert(mizu_pool_notify_arm(ctrl, 0) == MIZU_OK);
  assert(mizu_pool_stop(ctrl, 5000) == MIZU_OK);
  assert(wait_exit(w) == 0);
  mizu_pool_destroy(ctrl);
  puts("ok pool_cross_process");
}

/* The pool owner that dies with no workers: creates, hands the token
   over, then dies hard once the submitter is armed. With nobody left to
   broadcast, the armed submitter's chained owner watch must still fire. */
static void run_dying_owner(int token_wr, int go_rd) {
  mizu_binding b;
  mizu_binding_bytes(&b);
  mizu_pool_opts opts;
  mizu_pool_opts_init(&opts);
  opts.max_workers = 1;
  opts.max_submitters = 4;
  opts.injection_cap = 64;
  opts.per_worker_cap = 64;
  opts.result_slots = 64;
  opts.slot_size = 512;
  mizu_pool *o;
  if (mizu_pool_create(&o, &opts, &b) != MIZU_OK) _exit(3);
  char tok[64];
  if (mizu_pool_token(o, tok, sizeof(tok)) != MIZU_OK) _exit(4);
  if (write(token_wr, tok, strlen(tok) + 1) != (ssize_t) (strlen(tok) + 1))
    _exit(5);
  char go;
  if (read(go_rd, &go, 1) != 1) _exit(6);
  _exit(0);   /* hard: no stop, no destroy — and no worker ever joined */
}

static void test_pool_owner_death_chain(void) {
  int tok_pipe[2], go_pipe[2];
  assert(pipe(tok_pipe) == 0 && pipe(go_pipe) == 0);
  fflush(stdout);
  pid_t o = fork();
  assert(o >= 0);
  if (o == 0) {
    close(tok_pipe[0]);
    close(go_pipe[1]);
    run_dying_owner(tok_pipe[1], go_pipe[0]);
  }
  close(tok_pipe[1]);
  close(go_pipe[0]);
  assert(read(tok_pipe[0], token, sizeof(token)) > 0);
  close(tok_pipe[0]);

  mizu_binding b;
  mizu_binding_bytes(&b);
  mizu_pool *sub;
  assert(mizu_pool_attach(&sub, token, &b) == MIZU_OK);
  mizu_task t;
  mizu_bytes msg = { (void *) "t", 1 };
  assert(mizu_pool_submit(sub, &msg, &t, 1000) == MIZU_OK);
  fires = 0;
  assert(mizu_pool_task_notify(sub, &t, 1, 1) == MIZU_OK);
  assert(mizu_pool_notify_arm(sub, 1) == MIZU_OK);
  assert(mizu_pool_notify_watch(sub, count_cb, NULL) == MIZU_OK);

  /* the owner dies with every (never-joined) worker: nothing publishes,
     yet the chained watch fires the drain */
  char go = 'x';
  assert(write(go_pipe[1], &go, 1) == 1);
  close(go_pipe[1]);
  assert(wait_exit(o) == 0);
  assert(wait_fires(1, 5000));
  /* the sync half sees the verdict too: the collect terminates (the
     orphan teardown's cancel sweep ends the wait) */
  void *obj = NULL;
  assert(mizu_pool_collect(sub, &t, &obj, 5000) != MIZU_OK);
  assert(mizu_pool_notify_unwatch(sub) == MIZU_OK);
  mizu_pool_destroy(sub);
  puts("ok pool_owner_death_chain");
}

/* The armed peer that dies without a close: its endpoint path is the
   survivor's to unlink (with the .live.* files), not residue. */
static void run_armed_dying_peer(void) {
  mizu_binding b;
  mizu_binding_bytes(&b);
  mizu_channel *p;
  if (mizu_channel_attach(&p, token, &b) != MIZU_OK) _exit(3);
  if (mizu_channel_ready_set(p) != MIZU_OK) _exit(4);
  if (mizu_channel_notify_arm(p, 1, NULL) != MIZU_OK) _exit(5);
  _exit(0);   /* hard: no unwatch, no destroy — the endpoint path stays */
}

static void test_peer_endpoint_unlink(void) {
  char dir[] = "/tmp/mizu-test-ntfy-chan-XXXXXX";
  assert(mkdtemp(dir) != NULL);
  assert(setenv("MIZU_LIVENESS_DIR", dir, 1) == 0);
  mizu_binding b;
  mizu_binding_bytes(&b);
  mizu_channel_opts opts;
  mizu_channel_opts_init(&opts);
  opts.capacity = 64;
  opts.slot_size = 512;
  mizu_channel *host;
  assert(mizu_channel_create(&host, &opts, &b) == MIZU_OK);
  assert(mizu_channel_token(host, token, sizeof(token)) == MIZU_OK);
  mizu_channel_info info;
  assert(mizu_channel_info_get(host, &info) == MIZU_OK);

  fflush(stdout);
  pid_t p = fork();
  assert(p >= 0);
  if (p == 0) run_armed_dying_peer();
  assert(mizu_channel_ready_wait(host, 5000) == MIZU_OK);
  assert(wait_exit(p) == 0);
  /* the verdict runs the survivor unlink */
  void *obj = NULL;
  assert(mizu_channel_recv(host, &obj, 5000) == MIZU_PEER_GONE);

  char path[1200];
  struct stat st;
  snprintf(path, sizeof(path), "%s%s.ntfy.peer", dir, info.name);
  assert(stat(path, &st) != 0 && errno == ENOENT);
  snprintf(path, sizeof(path), "%s%s.live.peer", dir, info.name);
  assert(stat(path, &st) != 0 && errno == ENOENT);
  mizu_channel_destroy(host);
  unsetenv("MIZU_LIVENESS_DIR");
  assert(rmdir(dir) == 0);
  puts("ok peer_endpoint_unlink");
}

/* The armed submitter that dies without a leave: the controller's
   teardown unlinks its endpoint path beside the liveness files. The
   controller holds submitter slot 0, so the child claims slot 1. */
static void run_armed_dying_submitter(void) {
  mizu_binding b;
  mizu_binding_bytes(&b);
  mizu_pool *p;
  if (mizu_pool_attach(&p, token, &b) != MIZU_OK) _exit(3);
  if (mizu_pool_notify_arm(p, 1) != MIZU_OK) _exit(4);
  _exit(0);   /* hard: no disarm, no destroy — the endpoint path stays */
}

static void test_pool_endpoint_unlink(void) {
  char dir[] = "/tmp/mizu-test-ntfy-pool-XXXXXX";
  assert(mkdtemp(dir) != NULL);
  assert(setenv("MIZU_LIVENESS_DIR", dir, 1) == 0);
  mizu_binding b;
  mizu_binding_bytes(&b);
  mizu_pool_opts opts;
  mizu_pool_opts_init(&opts);
  opts.max_workers = 1;
  opts.max_submitters = 4;
  opts.injection_cap = 64;
  opts.per_worker_cap = 64;
  opts.result_slots = 64;
  opts.slot_size = 512;
  mizu_pool *ctrl;
  assert(mizu_pool_create(&ctrl, &opts, &b) == MIZU_OK);
  assert(mizu_pool_token(ctrl, token, sizeof(token)) == MIZU_OK);
  mizu_pool_status status;
  memset(&status, 0, sizeof(status));
  status.size = sizeof(status);
  assert(mizu_pool_status_get(ctrl, &status) == MIZU_OK);

  fflush(stdout);
  pid_t p = fork();
  assert(p >= 0);
  if (p == 0) run_armed_dying_submitter();
  assert(wait_exit(p) == 0);
  /* stop reaps the dead submitter and unlinks every endpoint tag */
  assert(mizu_pool_stop(ctrl, 5000) == MIZU_OK);
  char path[1200];
  struct stat st;
  snprintf(path, sizeof(path), "%s%s.ntfy.sub1", dir, status.name);
  assert(stat(path, &st) != 0 && errno == ENOENT);
  mizu_pool_destroy(ctrl);
  unsetenv("MIZU_LIVENESS_DIR");
  assert(rmdir(dir) == 0);
  puts("ok pool_endpoint_unlink");
}

int main(void) {
  test_cross_process_burst();
  test_death_chain();
  test_pool_cross_process();
  test_pool_owner_death_chain();
  test_peer_endpoint_unlink();
  test_pool_endpoint_unlink();
  puts("test_notify integration: all passed");
  return 0;
}

#endif
