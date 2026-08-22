/* Integration tier: the pool across real processes. Children are forked
   copies of this binary re-joining by token (fresh handles — pool handles
   do not survive fork); each child role ends in _exit. Scenarios: worker
   kill -9 mid-task -> DIED result, and stop over the dead worker answers
   REI_OK (the death watch's off-thread reap clears the slot during the
   wait); a killed worker's orphaned deque drains through a surviving
   peer; owner death -> worker exit (REI_EXIT_OWNER_GONE) + orphan
   teardown; submitter death -> registry-slot reclaim; stop with live
   workers through a collect_all round trip; the lame-duck linger.
   Compiles against rei.h only — the API's compile-time contract check.
   POSIX (fork); a stub passes elsewhere. Run via `make test-integration`. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rei.h"

#ifdef _WIN32

int main(void) {
  puts("test_pool integration: skipped on Windows (fork-based)");
  return 0;
}

#else

#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// Child plumbing ------------------------------------------------------------------

static char child_token[64];
static int child_role;
static uint32_t child_slot;
static rei_worker_exit child_expect;
static int child_linger;
static int go_pipe[2] = { -1, -1 };   /* parent -> child go-ahead */
static int joined_wr = -1;            /* child -> parent join signal */

enum { ROLE_WORKER, ROLE_NEST, ROLE_SUB_CANCEL, ROLE_SUB_ECHO };

static void sleep_ms(long ms) {
  struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
  nanosleep(&ts, NULL);
}

/* The child evaluator: echoes INLINE tasks; "die" kills the worker
   mid-task; "big" returns a 100 KB (spilled) result. */
static int child_exec(const rei_slot_hdr *hdr, const uint8_t *payload,
                      size_t limit, rei_result_sink *sink, int catching,
                      void *ctx) {
  (void) limit; (void) catching; (void) ctx;
  if (hdr->kind == REI_KIND_NIL) {
    rei_bytes b = { NULL, 0 };
    return rei_result_publish(sink, &b) >= 0 ? 0 : 1;
  }
  assert(hdr->kind == REI_KIND_INLINE);
  if (hdr->len == 3 && memcmp(payload, "die", 3) == 0)
    raise(SIGKILL);
  if (hdr->len == 3 && memcmp(payload, "big", 3) == 0) {
    static unsigned char big[100 * 1024];
    for (size_t i = 0; i < sizeof big; i++) big[i] = (unsigned char) (i * 31u);
    rei_bytes b = { big, sizeof big };
    return rei_result_publish(sink, &b) >= 0 ? 0 : 1;
  }
  rei_bytes b = { (void *) payload, hdr->len };
  return rei_result_publish(sink, &b) >= 0 ? 0 : 1;
}

static void child_binding(rei_binding *b, int worker) {
  rei_binding_bytes(b);
  if (worker) b->exec = child_exec;
}

/* Join + run; leave, linger if asked, and exit 0 on the expected reason. */
static void run_worker(uint32_t slot, rei_worker_exit expect, int linger) {
  rei_binding b;
  child_binding(&b, 1);
  rei_pool *p;
  if (rei_pool_worker_join(&p, child_token, slot, &b) != REI_OK) _exit(3);
  if (joined_wr >= 0) {
    char c = 'j';
    (void) write(joined_wr, &c, 1);
  }
  rei_worker_exit ex = rei_pool_worker_run(p);
  if (ex != expect) _exit(4);
  if (rei_pool_leave(p) != REI_OK) _exit(5);
  if (linger)
    while (!rei_pool_lame_duck(p)) sleep_ms(10);
  rei_pool_destroy(p);
  _exit(0);
}

/* Nested-submit four tasks onto the own deque, wait the go-ahead, die
   with them queued. */
static void run_nest(uint32_t slot) {
  rei_binding b;
  child_binding(&b, 1);
  rei_pool *p;
  if (rei_pool_worker_join(&p, child_token, slot, &b) != REI_OK) _exit(3);
  const char *names[4] = { "n0", "n1", "n2", "n3" };
  for (int i = 0; i < 4; i++) {
    rei_bytes tb = { (void *) names[i], 2 };
    rei_task t;
    if (rei_pool_submit(p, &tb, &t, 0) != REI_OK) _exit(6);
  }
  char c;
  if (read(go_pipe[0], &c, 1) != 1) _exit(7);
  raise(SIGKILL);
  _exit(8);
}

/* Attach, submit, cancel, exit — dead with a cancelled task queued. */
static void run_submitter_cancel(void) {
  rei_binding b;
  child_binding(&b, 0);
  rei_pool *p;
  if (rei_pool_attach(&p, child_token, &b) != REI_OK) _exit(3);
  rei_bytes tb = { (void *) "x", 1 };
  rei_task t;
  if (rei_pool_submit(p, &tb, &t, 0) != REI_OK) _exit(6);
  if (rei_pool_cancel(p, &t) != 1) _exit(7);
  _exit(0);   /* no destroy: the death is the point */
}

/* Attach, submit, collect and check — the reclaimed slot serves. */
static void run_submitter_echo(void) {
  rei_binding b;
  child_binding(&b, 0);
  rei_pool *p;
  if (rei_pool_attach(&p, child_token, &b) != REI_OK) _exit(3);
  rei_bytes tb = { (void *) "y", 1 };
  rei_task t;
  if (rei_pool_submit(p, &tb, &t, 1000) != REI_OK) _exit(6);
  void *obj = NULL;
  if (rei_pool_collect(p, &t, &obj, 5000) != REI_OK) _exit(7);
  rei_bytes *rb = obj;
  int ok = rb != NULL && rb->len == 1 && memcmp(rb->data, "y", 1) == 0;
  rei_bytes_free(rb);
  rei_pool_destroy(p);
  _exit(ok ? 0 : 8);
}

static pid_t spawn(int role, uint32_t slot) {
  fflush(stdout);
  child_role = role;
  child_slot = slot;
  pid_t pid = fork();
  assert(pid >= 0);
  if (pid == 0) {
    switch (child_role) {
    case ROLE_WORKER: run_worker(child_slot, child_expect, child_linger); break;
    case ROLE_NEST: run_nest(child_slot); break;
    case ROLE_SUB_CANCEL: run_submitter_cancel(); break;
    case ROLE_SUB_ECHO: run_submitter_echo(); break;
    }
    _exit(2);
  }
  return pid;
}

static int wait_exit(pid_t pid) {
  int st = 0;
  assert(waitpid(pid, &st, 0) == pid);
  return WIFEXITED(st) ? WEXITSTATUS(st) : 128;
}

static int wait_signalled(pid_t pid) {
  int st = 0;
  assert(waitpid(pid, &st, 0) == pid);
  return WIFSIGNALED(st) ? WTERMSIG(st) : 0;
}

// Harness ------------------------------------------------------------------------------

static rei_pool *make_pool(uint32_t workers, uint32_t subs) {
  rei_pool_opts opts;
  rei_pool_opts_init(&opts);
  opts.max_workers = workers;
  opts.max_submitters = subs;
  opts.injection_cap = 8;
  opts.per_worker_cap = 8;
  opts.result_slots = 8 * subs;
  opts.slot_size = 512;
  rei_binding b;
  rei_binding_bytes(&b);
  rei_pool *p;
  assert(rei_pool_create(&p, &opts, &b) == REI_OK);
  assert(rei_pool_token(p, child_token, sizeof child_token) == REI_OK);
  return p;
}

// Scenarios ------------------------------------------------------------------------------

/* Worker kill -9 mid-task: the result fails DIED (the death watch's
   off-thread reap, or the collect wake-backstop, runs the verdict), and
   stop over the dead worker answers REI_OK — the slot is already reaped. */
static void test_worker_death(void) {
  rei_pool *ctrl = make_pool(1, 2);
  child_expect = REI_EXIT_SHUTDOWN;
  child_linger = 0;
  pid_t w = spawn(ROLE_WORKER, 0);
  uint32_t slot0 = 0;
  assert(rei_pool_ready_wait(ctrl, &slot0, 1, 5000) == REI_OK);

  rei_bytes tb = { (void *) "die", 3 };
  rei_task t;
  assert(rei_pool_submit(ctrl, &tb, &t, 1000) == REI_OK);
  void *obj = NULL;
  assert(rei_pool_collect(ctrl, &t, &obj, 10000) == REI_ERR);
  assert(obj == NULL);
  assert(strcmp(rei_pool_error(ctrl),
                "worker died while executing this task") == 0);
  assert(wait_signalled(w) == SIGKILL);

  assert(rei_pool_stop(ctrl, 5000) == REI_OK);
  rei_pool_destroy(ctrl);
  puts("ok worker_death");
}

/* A killed worker's orphaned deque drains through a surviving peer: the
   four nested tasks' results publish into the dead worker's submitter
   subrange. */
static void test_orphan_deque_drain(void) {
  rei_pool *ctrl = make_pool(2, 2);
  assert(pipe(go_pipe) == 0);
  pid_t w0 = spawn(ROLE_NEST, 0);
  child_expect = REI_EXIT_SHUTDOWN;
  child_linger = 0;
  pid_t w1 = spawn(ROLE_WORKER, 1);
  uint32_t slots[2] = { 0, 1 };
  assert(rei_pool_ready_wait(ctrl, slots, 2, 5000) == REI_OK);
  assert(write(go_pipe[1], "g", 1) == 1);   /* w0 dies with a queued deque */
  close(go_pipe[0]);
  close(go_pipe[1]);
  go_pipe[0] = go_pipe[1] = -1;
  assert(wait_signalled(w0) == SIGKILL);

  /* w1 steals and executes the four orphaned nested tasks */
  rei_rs_row rows[16];
  int done = 0;
  for (int i = 0; i < 2000 && !done; i++) {
    uint32_t nrows = 0;
    assert(rei_pool_tasks_get(ctrl, rows, 16, &nrows) == REI_OK);
    done = nrows == 4;
    for (uint32_t k = 0; k < nrows && k < 4; k++)
      done = done && rows[k].status == REI_RS_OK;
    if (!done) sleep_ms(5);
  }
  assert(done);

  assert(rei_pool_stop(ctrl, 5000) == REI_OK);
  assert(wait_exit(w1) == 0);
  rei_pool_destroy(ctrl);
  puts("ok orphan_deque_drain");
}

/* Owner death: the worker's owner watch ends its run OWNER_GONE and the
   first detector tears the orphan pool down — the region is unlinked, so
   a late attach fails. */
static void test_owner_death(void) {
  int tok[2], go[2], joined[2];
  assert(pipe(tok) == 0 && pipe(go) == 0 && pipe(joined) == 0);
  fflush(stdout);
  pid_t a = fork();
  assert(a >= 0);
  if (a == 0) {
    close(tok[0]);
    close(go[1]);
    close(joined[0]);
    close(joined[1]);
    rei_pool_opts opts;
    rei_pool_opts_init(&opts);
    opts.max_workers = 1;
    opts.max_submitters = 2;
    opts.injection_cap = 8;
    opts.per_worker_cap = 8;
    opts.result_slots = 16;
    opts.slot_size = 512;
    rei_binding b;
    rei_binding_bytes(&b);
    rei_pool *p;
    if (rei_pool_create(&p, &opts, &b) != REI_OK) _exit(3);
    char token[64];
    if (rei_pool_token(p, token, sizeof token) != REI_OK) _exit(4);
    if (write(tok[1], token, sizeof token) != (ssize_t) sizeof token)
      _exit(5);
    char c;
    if (read(go[0], &c, 1) != 1) _exit(6);
    _exit(0);   /* the owner exits without stop: pool lifetime == creator */
  }
  close(tok[1]);
  close(go[0]);
  assert(read(tok[0], child_token, sizeof child_token) ==
         (ssize_t) sizeof child_token);
  close(tok[0]);

  child_expect = REI_EXIT_OWNER_GONE;
  child_linger = 0;
  joined_wr = joined[1];
  pid_t b_pid = spawn(ROLE_WORKER, 0);
  close(joined[1]);
  char c;
  assert(read(joined[0], &c, 1) == 1);   /* joined while the owner lives */
  close(joined[0]);
  joined_wr = -1;
  assert(write(go[1], "g", 1) == 1);     /* let the owner exit */
  close(go[1]);
  assert(wait_exit(a) == 0);
  assert(wait_exit(b_pid) == 0);

  rei_binding bb;
  rei_binding_bytes(&bb);
  rei_pool *late;
  assert(rei_pool_attach(&late, child_token, &bb) == REI_ERR);
  puts("ok owner_death");
}

/* Submitter death: the worker's CANCEL-consume probes and reaps the dead
   submitter in-line, freeing its registry slot for a fresh attach. */
static void test_submitter_death(void) {
  rei_pool *ctrl = make_pool(1, 2);
  pid_t s = spawn(ROLE_SUB_CANCEL, 0);
  assert(wait_exit(s) == 0);   /* dead with a cancelled task queued */

  child_expect = REI_EXIT_SHUTDOWN;
  child_linger = 0;
  pid_t w = spawn(ROLE_WORKER, 0);
  uint32_t slot0 = 0;
  assert(rei_pool_ready_wait(ctrl, &slot0, 1, 5000) == REI_OK);

  /* the claim consumes the CANCEL and reaps the dead submitter */
  int freed = 0;
  for (int i = 0; i < 2000 && !freed; i++) {
    rei_pool_status st;
    assert(rei_pool_status_get(ctrl, &st) == REI_OK);
    freed = st.sub_state[1] == REI_SUB_FREE;
    if (!freed) sleep_ms(5);
  }
  assert(freed);

  pid_t s2 = spawn(ROLE_SUB_ECHO, 0);   /* the reclaimed slot serves */
  assert(wait_exit(s2) == 0);

  assert(rei_pool_stop(ctrl, 5000) == REI_OK);
  assert(wait_exit(w) == 0);
  rei_pool_destroy(ctrl);
  puts("ok submitter_death");
}

/* Stop with live workers: a cross-process collect_all round trip, then a
   clean shutdown — both workers take their exit path inside the wait. */
static void test_stop_live_workers(void) {
  rei_pool *ctrl = make_pool(2, 2);
  child_expect = REI_EXIT_SHUTDOWN;
  child_linger = 0;
  pid_t w0 = spawn(ROLE_WORKER, 0);
  pid_t w1 = spawn(ROLE_WORKER, 1);
  uint32_t slots[2] = { 0, 1 };
  assert(rei_pool_ready_wait(ctrl, slots, 2, 5000) == REI_OK);

  const char *words[4] = { "aa", "bb", "cc", "dd" };
  rei_task ts[4];
  for (int i = 0; i < 4; i++) {
    rei_bytes b = { (void *) words[i], 2 };
    assert(rei_pool_submit(ctrl, &b, &ts[i], 5000) == REI_OK);
  }
  void *vals[4] = { NULL, NULL, NULL, NULL };
  size_t err = 99;
  assert(rei_pool_collect_all(ctrl, ts, 4, vals, &err, 10000) == REI_OK);
  assert(err == 4);
  for (int i = 0; i < 4; i++) {
    rei_bytes *b = vals[i];
    assert(b->len == 2 && memcmp(b->data, words[i], 2) == 0);
    rei_bytes_free(b);
  }

  assert(rei_pool_stop(ctrl, 5000) == REI_OK);
  assert(wait_exit(w0) == 0);
  assert(wait_exit(w1) == 0);
  rei_pool_destroy(ctrl);
  puts("ok stop_live_workers");
}

/* The lame-duck linger: a retired worker with an uncollected spilled
   result stays alive as its lifetime anchor until the collect frees the
   slot. */
static void test_lame_duck(void) {
  rei_pool *ctrl = make_pool(1, 2);
  child_expect = REI_EXIT_RETIRED;
  child_linger = 1;
  pid_t w = spawn(ROLE_WORKER, 0);
  uint32_t slot0 = 0;
  assert(rei_pool_ready_wait(ctrl, &slot0, 1, 5000) == REI_OK);

  rei_bytes tb = { (void *) "big", 3 };
  rei_task t;
  assert(rei_pool_submit(ctrl, &tb, &t, 1000) == REI_OK);
  int ok = 0;
  for (int i = 0; i < 2000 && !ok; i++) {
    ok = rei_pool_task_state(ctrl, &t) == REI_RS_OK;
    if (!ok) sleep_ms(5);
  }
  assert(ok);

  assert(rei_pool_retire(ctrl, 0) == REI_OK);
  /* the worker retires and lingers — it cannot exit while the result is
     uncollected */
  sleep_ms(300);
  int st;
  assert(waitpid(w, &st, WNOHANG) == 0);

  void *obj = NULL;
  assert(rei_pool_collect(ctrl, &t, &obj, 5000) == REI_OK);
  rei_bytes *b = obj;
  assert(b->len == 100 * 1024);
  assert(((unsigned char *) b->data)[0] == 0);
  assert(((unsigned char *) b->data)[99999] == (unsigned char) (99999 * 31u));
  rei_bytes_free(b);

  assert(wait_exit(w) == 0);   /* the linger ended once the slot freed */
  assert(rei_pool_stop(ctrl, 5000) == REI_OK);
  rei_pool_destroy(ctrl);
  puts("ok lame_duck");
}

int main(void) {
  test_worker_death();
  test_orphan_deque_drain();
  test_owner_death();
  test_submitter_death();
  test_stop_live_workers();
  test_lame_duck();
  puts("test_pool integration: all passed");
  return 0;
}

#endif
