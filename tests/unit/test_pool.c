/* Unit tier: the pool transport, exercised in-process as a controller
   plus worker/submitter handles in one process (the pool_pair/pool_step
   discipline), staging through the built-in bytes binding with a test
   exec_fn standing in for a language evaluator. Worker death is
   simulated by destroying a worker handle without leave (its liveness
   lock releases; the slot still reads LIVE — exactly a crash), so the
   reaper runs synchronously off the collect/stop probe paths.
   Assert-based; run via `make test`. */

#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

// The test binding: bytes stage/read plus a modal exec -------------------------------

static rei_binding bytesb;
static int exec_mode = 0;  /* 0 echo, 1 strand (no publish), 2 infra fail,
                              3 abandon (eval_mark + longjmp), 4 ERR inline,
                              5 ERR tiered */
static int err_read = 0;   /* read_fn returns the ERR envelope's bytes */
static int exec_calls = 0;
static jmp_buf abandon_jmp;

/* Resolve a task frame's bytes: INLINE/NIL directly, SHM_RAW through the
   read-side region service (the worker handle's open cache). */
static void task_bytes(rei_pool *p, const rei_slot_hdr *hdr,
                       const uint8_t *payload, size_t limit,
                       const uint8_t **bytes, size_t *n) {
  switch (hdr->kind) {
  case REI_KIND_NIL:
    *bytes = NULL;
    *n = 0;
    return;
  case REI_KIND_INLINE:
    assert(hdr->len <= limit);
    *bytes = payload;
    *n = hdr->len;
    return;
  case REI_KIND_SHM_RAW: {
    rei_read_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.size = (uint32_t) sizeof(ctx);
    ctx.outcome = REI_RS_OK;
    ctx.died_slot = -1;
    ctx.handle = (rei_handle *) p;   /* the handle base is the first member */
    rei_shm *shm = rei_read_region(&ctx, payload, hdr->len);
    assert(shm != NULL);
    assert(hdr->aux <= (uint64_t) shm->size);
    *bytes = shm->addr;
    *n = (size_t) hdr->aux;
    return;
  }
  default:
    assert(0);   /* no other tier appears in these tests */
  }
}

static int test_exec(const rei_slot_hdr *hdr, const uint8_t *payload,
                     size_t limit, rei_result_sink *sink, int catching,
                     void *ctx) {
  (void) ctx;
  /* payload-keyed outcome, ahead of the mode dispatch: "err" publishes
     the INLINE-framed ERR envelope */
  if (hdr->kind == REI_KIND_INLINE && hdr->len == 3 &&
      memcmp(payload, "err", 3) == 0) {
    static const char msg[] = "boom";
    assert(sizeof(msg) <= sink->inline_max);
    memcpy(sink->payload, msg, sizeof(msg));
    assert(rei_result_publish_err(sink, NULL, (uint32_t) sizeof(msg)) == 1);
    return 0;
  }
  switch (exec_mode) {
  case 1:
    return 0;   /* claim, never publish: strands the in-flight task */
  case 2:
    return 1;   /* infrastructure failure: takes the worker down */
  case 3:
    if (!catching) {
      rei_pool_eval_mark(sink->p, 1);
      longjmp(abandon_jmp, 1);   /* the binding's unwind path (R's longjmp) */
    }
    break;
  case 4: {
    /* the ERR envelope framed INLINE in the sink's frame buffer */
    static const char msg[] = "boom";
    assert(sizeof(msg) <= sink->inline_max);
    memcpy(sink->payload, msg, sizeof(msg));
    assert(rei_result_publish_err(sink, NULL, (uint32_t) sizeof(msg)) == 1);
    return 0;
  }
  case 5: {
    /* the ERR envelope riding the tiered stage */
    rei_bytes b = { (void *) "boom-tiered", 11 };
    assert(rei_result_publish_err(sink, &b, 0) == 1);
    return 0;
  }
  default:
    break;
  }
  /* echo: the task bytes come back as the result */
  const uint8_t *bytes = NULL;   /* MinGW's assert is not noreturn, so */
  size_t n = 0;                  /* task_bytes' default case warns */
  task_bytes(sink->p, hdr, payload, limit, &bytes, &n);
  rei_bytes b = { (void *) bytes, n };
  assert(rei_result_publish(sink, &b) == 1);
  exec_calls++;
  return 0;
}

static void *test_read(const rei_slot_hdr *hdr, const uint8_t *payload,
                       size_t limit, rei_read_ctx *ctx) {
  if (err_read && ctx->outcome == REI_RS_ERR) {
    /* read the envelope's bytes as if OK: verifies the framing crossed */
    rei_read_ctx tmp = *ctx;
    tmp.outcome = REI_RS_OK;
    return bytesb.read(hdr, payload, limit, &tmp);
  }
  return bytesb.read(hdr, payload, limit, ctx);
}

static int trace_events[16];
static int trace_n;
static void trace_cb(rei_trace_event ev, uint64_t task_id, void *ctx) {
  (void) task_id;
  (void) ctx;
  if (trace_n < 16) trace_events[trace_n++] = (int) ev;
}

// Harness ------------------------------------------------------------------------------

static rei_pool *ctrl, *wk, *wk2, *sub;
static char token[64];

static void make_binding(rei_binding *b, int worker) {
  rei_binding_bytes(b);
  b->read = test_read;
  if (worker) b->exec = test_exec;
}

/* Controller plus `joined` worker handles (of `workers` slots), all
   in-process. */
static void pool_pair(uint32_t workers, uint32_t joined, uint32_t max_sub,
                      uint32_t inj, uint32_t dq, uint32_t rs, uint32_t slot) {
  rei_pool_opts opts;
  rei_pool_opts_init(&opts);
  opts.max_workers = workers;
  opts.max_submitters = max_sub;
  opts.injection_cap = inj;
  opts.per_worker_cap = dq;
  opts.result_slots = rs;
  opts.slot_size = slot;
  rei_binding b, wb;
  make_binding(&b, 0);
  make_binding(&wb, 1);
  assert(rei_pool_create(&ctrl, &opts, &b) == REI_OK);
  assert(rei_pool_token(ctrl, token, sizeof(token)) == REI_OK);
  assert(joined >= 1 && joined <= workers && joined <= 2);
  assert(rei_pool_worker_join(&wk, token, 0, &wb) == REI_OK);
  wk2 = NULL;
  if (joined > 1)
    assert(rei_pool_worker_join(&wk2, token, 1, &wb) == REI_OK);
  sub = NULL;
}

static void pool_end(void) {
  if (sub != NULL) {
    rei_pool_destroy(sub);
    sub = NULL;
  }
  if (wk2 != NULL) {
    assert(rei_pool_leave(wk2) == REI_OK);
    rei_pool_destroy(wk2);
    wk2 = NULL;
  }
  if (wk != NULL) {
    assert(rei_pool_leave(wk) == REI_OK);
    rei_pool_destroy(wk);
    wk = NULL;
  }
  rei_pool_destroy(ctrl);
  ctrl = NULL;
}

static rei_task submit_bytes(rei_pool *p, const void *data, size_t len) {
  rei_bytes b = { (void *) data, len };
  rei_task t;
  assert(rei_pool_submit(p, &b, &t, 1000) == REI_OK);
  return t;
}

static void collect_bytes(rei_pool *p, rei_task *t, const void *expect,
                          size_t len) {
  void *obj = NULL;
  assert(rei_pool_collect(p, t, &obj, 1000) == REI_OK);
  rei_bytes *b = obj;
  assert(b->len == len);
  assert(len == 0 || memcmp(b->data, expect, len) == 0);
  rei_bytes_free(b);
}

// Scenarios ------------------------------------------------------------------------------

/* Round trip: INLINE and NIL tasks, submit -> step -> collect. */
static void test_round_trip(void) {
  pool_pair(1, 1, 8, 64, 64, 64, 512);
  rei_task t = submit_bytes(ctrl, "hello", 5);
  assert(rei_pool_task_state(ctrl, &t) == REI_RS_PENDING);
  assert(rei_pool_step(wk, 0) == REI_STEP_TASK);
  assert(rei_pool_task_state(ctrl, &t) == REI_RS_OK);
  collect_bytes(ctrl, &t, "hello", 5);
  assert(rei_pool_task_state(ctrl, &t) == REI_RS_FREE);   /* collected */

  rei_task t2 = submit_bytes(ctrl, NULL, 0);
  assert(rei_pool_step(wk, 0) == REI_STEP_TASK);
  collect_bytes(ctrl, &t2, NULL, 0);
  pool_end();
  puts("ok round_trip");
}

/* Spill tiers both ways: a 100 KB task and its 100 KB result ride
   SHM_RAW regions; the second large submit recycles the first's
   surrendered region (the submitter's free-list hit). */
static void test_spill(void) {
  pool_pair(1, 1, 8, 64, 64, 64, 512);
  size_t n = 100 * 1024;
  unsigned char *big = malloc(n);
  for (size_t i = 0; i < n; i++) big[i] = (unsigned char) (i * 31u);

  rei_task t = submit_bytes(ctrl, big, n);
  assert(rei_pool_step(wk, 0) == REI_STEP_TASK);
  collect_bytes(ctrl, &t, big, n);

  uint64_t hits_before = ((rei_handle *) ctrl)->fl.hits;
  rei_task t2 = submit_bytes(ctrl, big, n);
  assert(((rei_handle *) ctrl)->fl.hits > hits_before);
  assert(rei_pool_step(wk, 0) == REI_STEP_TASK);
  collect_bytes(ctrl, &t2, big, n);
  free(big);
  pool_end();
  puts("ok spill");
}

/* Batches: a burst larger than the ring ends early at the deadline
   (REI_OK, *n_out < n); the single form answers REI_FULL. */
static void test_batch(void) {
  pool_pair(1, 1, 8, 2, 64, 64, 512);   /* injection_cap = 2 */
  rei_bytes objs[4] = { { (void *) "a", 1 }, { (void *) "b", 1 },
                        { (void *) "c", 1 }, { (void *) "d", 1 } };
  void *objs_p[4] = { &objs[0], &objs[1], &objs[2], &objs[3] };
  rei_task ts[4];
  size_t n_out = 99;
  assert(rei_pool_submit_batch(ctrl, objs_p, 4, ts, &n_out, 0) == REI_OK);
  assert(n_out == 2);

  rei_bytes e = { (void *) "e", 1 };
  rei_task te;
  assert(rei_pool_submit(ctrl, &e, &te, 0) == REI_FULL);

  assert(rei_pool_step(wk, 0) == REI_STEP_TASK);
  assert(rei_pool_step(wk, 0) == REI_STEP_TASK);
  collect_bytes(ctrl, &ts[0], "a", 1);
  collect_bytes(ctrl, &ts[1], "b", 1);

  assert(rei_pool_submit(ctrl, &e, &te, 1000) == REI_OK);
  assert(rei_pool_step(wk, 0) == REI_STEP_TASK);
  collect_bytes(ctrl, &te, "e", 1);
  pool_end();
  puts("ok batch");
}

/* Result-slot exhaustion raises (REI_ERRCAT_EXHAUSTED), distinct from a
   full ring (REI_FULL). */
static void test_exhaustion(void) {
  pool_pair(1, 1, 2, 64, 64, 4, 512);   /* 2 submitters, 2 slots each */
  rei_task a = submit_bytes(ctrl, "a", 1);
  rei_task b = submit_bytes(ctrl, "b", 1);
  rei_bytes c = { (void *) "c", 1 };
  rei_task tc;
  assert(rei_pool_submit(ctrl, &c, &tc, 0) == REI_ERR);
  assert(rei_pool_errcat(ctrl) == REI_ERRCAT_EXHAUSTED);

  assert(rei_pool_step(wk, 0) == REI_STEP_TASK);
  collect_bytes(ctrl, &a, "a", 1);
  assert(rei_pool_submit(ctrl, &c, &tc, 1000) == REI_OK);
  assert(rei_pool_step(wk, 0) == REI_STEP_TASK);   /* b */
  collect_bytes(ctrl, &b, "b", 1);
  assert(rei_pool_step(wk, 0) == REI_STEP_TASK);   /* c */
  collect_bytes(ctrl, &tc, "c", 1);
  pool_end();
  puts("ok exhaustion");
}

/* Cancel: advisory, discard-only. A cancelled task collects as REI_ERR
   (the bytes binding builds no error object); the worker's claim skips it
   without executing. */
static void test_cancel(void) {
  pool_pair(1, 1, 8, 64, 64, 64, 512);
  rei_task t = submit_bytes(ctrl, "x", 1);
  assert(rei_pool_cancel(ctrl, &t) == 1);
  assert(rei_pool_task_state(ctrl, &t) == REI_RS_CANCEL);
  assert(rei_pool_cancel(ctrl, &t) == 0);   /* already cancelled */

  void *obj = NULL;
  assert(rei_pool_collect(ctrl, &t, &obj, 1000) == REI_ERR);
  assert(obj == NULL);
  assert(strcmp(rei_pool_error(ctrl), "task cancelled or pool stopped") == 0);

  exec_calls = 0;
  assert(rei_pool_step(wk, 0) == REI_STEP_TASK);   /* claims, skips */
  assert(exec_calls == 0);

  rei_task t2 = submit_bytes(ctrl, "y", 1);
  assert(rei_pool_step(wk, 0) == REI_STEP_TASK);
  assert(rei_pool_cancel(ctrl, &t2) == 0);   /* too late: completed */
  assert(rei_pool_task_state(ctrl, &t2) == REI_RS_OK);  /* left collectible */
  collect_bytes(ctrl, &t2, "y", 1);

  /* the finalizer release: a terminal, uncollected slot is freed (0 —
     released, not cancelled) so the result never lingers until reuse */
  rei_task t3 = submit_bytes(ctrl, "z", 1);
  assert(rei_pool_step(wk, 0) == REI_STEP_TASK);
  assert(rei_pool_task_state(ctrl, &t3) == REI_RS_OK);
  assert(rei_pool_task_release(ctrl, &t3) == 0);
  assert(rei_pool_task_state(ctrl, &t3) == REI_RS_FREE);
  /* a pending task is cancelled through the same verb */
  rei_task t4 = submit_bytes(ctrl, "w", 1);
  assert(rei_pool_task_release(ctrl, &t4) == 1);
  assert(rei_pool_task_state(ctrl, &t4) == REI_RS_CANCEL);
  pool_end();
  puts("ok cancel");
}

/* A second submitter joins by token; malformed and unknown tokens are
   refused. */
static void test_attach(void) {
  pool_pair(1, 1, 8, 64, 64, 64, 512);
  rei_binding b;
  make_binding(&b, 0);
  assert(rei_pool_attach(&sub, token, &b) == REI_OK);
  rei_task t = submit_bytes(sub, "from-sub", 8);
  assert(rei_pool_step(wk, 0) == REI_STEP_TASK);
  collect_bytes(sub, &t, "from-sub", 8);

  rei_pool *bad;
  assert(rei_pool_attach(&bad, "nonsense!", &b) == REI_ERR);
  assert(rei_pool_attach(&bad, "deadbeef_deadbeef", &b) == REI_ERR);
  pool_end();
  puts("ok attach");
}

/* Stop: cancels pending tasks (a parked collect would wake to CANCEL),
   refuses new submits and late attaches, is idempotent. */
static void test_stop(void) {
  pool_pair(1, 1, 8, 64, 64, 64, 512);
  rei_binding b;
  make_binding(&b, 0);
  assert(rei_pool_attach(&sub, token, &b) == REI_OK);
  rei_task t = submit_bytes(sub, "doomed", 6);   /* never run */

  assert(rei_pool_leave(wk) == REI_OK);   /* the worker exits cleanly first */
  assert(rei_pool_stop(ctrl, 1000) == REI_OK);

  void *obj = NULL;
  assert(rei_pool_collect(sub, &t, &obj, 1000) == REI_ERR);
  assert(strcmp(rei_pool_error(sub), "task cancelled or pool stopped") == 0);

  rei_bytes z = { (void *) "z", 1 };
  rei_task tz;
  assert(rei_pool_submit(sub, &z, &tz, 0) == REI_ERR);
  assert(rei_pool_errcat(sub) == REI_ERRCAT_STOPPED);

  rei_pool *late;
  assert(rei_pool_attach(&late, token, &b) == REI_ERR);

  assert(rei_pool_stop(ctrl, 0) == REI_OK);   /* no-op */
  rei_pool_destroy(sub);
  sub = NULL;
  rei_pool_destroy(wk);
  wk = NULL;
  rei_pool_destroy(ctrl);
  ctrl = NULL;
  puts("ok stop");
}

/* Steal: a nested submit queues on the worker's own deque; a peer's step
   steals and executes it. */
static void test_steal(void) {
  pool_pair(2, 2, 8, 64, 64, 64, 512);
  rei_bytes b = { (void *) "nested", 6 };
  rei_task t;
  assert(rei_pool_submit(wk, &b, &t, 0) == REI_OK);   /* onto wk's deque */
  exec_calls = 0;
  assert(rei_pool_step(wk2, 0) == REI_STEP_TASK);     /* stolen by wk2 */
  assert(exec_calls == 1);
  collect_bytes(wk, &t, "nested", 6);   /* nested tasks collect on wk */
  pool_end();
  puts("ok steal");
}

/* Nested submit with a full deque runs the task inline; a nested collect
   helps (executes its own queued work) instead of parking. */
static void test_nested(void) {
  pool_pair(1, 1, 8, 64, 2, 64, 512);   /* per_worker_cap = 2 */
  rei_bytes b = { (void *) "n", 1 };
  rei_task t1, t2, t3;
  assert(rei_pool_submit(wk, &b, &t1, 0) == REI_OK);   /* deque: 1 */
  assert(rei_pool_submit(wk, &b, &t2, 0) == REI_OK);   /* deque: 2 (full) */
  exec_calls = 0;
  assert(rei_pool_submit(wk, &b, &t3, 0) == REI_OK);   /* inline execute */
  assert(exec_calls == 1);
  collect_bytes(wk, &t3, "n", 1);   /* already published */
  collect_bytes(wk, &t1, "n", 1);   /* help pops + runs both queued */
  collect_bytes(wk, &t2, "n", 1);
  assert(exec_calls == 3);
  pool_end();
  puts("ok nested");
}

/* The ERR publish: the envelope crosses INLINE (and tiered), the binding
   reads it as an error object; the bytes binding surfaces REI_ERR. */
static void test_err_publish(void) {
  pool_pair(1, 1, 8, 64, 64, 64, 512);
  void *obj = NULL;

  exec_mode = 4;
  rei_task t = submit_bytes(ctrl, "x", 1);
  assert(rei_pool_step(wk, 0) == REI_STEP_TASK);
  assert(rei_pool_task_state(ctrl, &t) == REI_RS_ERR);
  err_read = 1;
  assert(rei_pool_collect(ctrl, &t, &obj, 1000) == REI_OK);
  rei_bytes *eb = obj;
  assert(eb->len == 5 && memcmp(eb->data, "boom", 4) == 0);
  rei_bytes_free(eb);
  err_read = 0;

  exec_mode = 5;
  rei_task t2 = submit_bytes(ctrl, "x", 1);
  assert(rei_pool_step(wk, 0) == REI_STEP_TASK);
  err_read = 1;
  assert(rei_pool_collect(ctrl, &t2, &obj, 1000) == REI_OK);
  eb = obj;
  assert(eb->len == 11 && memcmp(eb->data, "boom-tiered", 11) == 0);
  rei_bytes_free(eb);
  err_read = 0;

  exec_mode = 4;
  rei_task t3 = submit_bytes(ctrl, "x", 1);
  assert(rei_pool_step(wk, 0) == REI_STEP_TASK);
  assert(rei_pool_collect(ctrl, &t3, &obj, 1000) == REI_ERR);
  assert(strcmp(rei_pool_error(ctrl), "task failed") == 0);
  exec_mode = 0;
  pool_end();
  puts("ok err_publish");
}

/* An exec_fn infrastructure failure takes the worker down
   (REI_STEP_SHUTDOWN, the error recorded); the stranded in-flight task
   fails DIED once the reaper confirms the worker's death — here via the
   collect wake-backstop probe after a simulated crash. */
static void test_infra_failure_and_reap(void) {
  pool_pair(1, 1, 8, 64, 64, 64, 512);
  exec_mode = 2;
  rei_task t = submit_bytes(ctrl, "x", 1);
  assert(rei_pool_step(wk, 0) == REI_STEP_SHUTDOWN);
  assert(rei_pool_errcat(wk) == REI_ERRCAT_OTHER);

  rei_pool_destroy(wk);   /* simulated crash: lock releases, slot LIVE */
  wk = NULL;
  void *obj = NULL;
  assert(rei_pool_collect(ctrl, &t, &obj, 500) == REI_ERR);
  assert(strcmp(rei_pool_error(ctrl),
                "worker died while executing this task") == 0);
  exec_mode = 0;
  pool_end();
  puts("ok infra_failure_and_reap");
}

/* The unwind path: an abandoned catching = 0 eval (a binding's longjmp)
   mints the in-flight task's sink through rei_pool_unwind_sink for the
   ERR publish; the worker loop heals and continues. */
static void test_unwind(void) {
  pool_pair(1, 1, 8, 64, 64, 64, 512);
  exec_mode = 3;
  rei_task t = submit_bytes(ctrl, "x", 1);
  if (setjmp(abandon_jmp) == 0) {
    rei_pool_step(wk, 0);
    assert(0);   /* the exec abandons: step never returns */
  }
  rei_result_sink sink;
  assert(rei_pool_unwind_sink(wk, &sink) == 1);
  static const char emsg[] = "eval blew up";
  assert(sizeof(emsg) <= sink.inline_max);
  memcpy(sink.payload, emsg, sizeof(emsg));
  assert(rei_result_publish_err(&sink, NULL, (uint32_t) sizeof(emsg)) == 1);
  assert(rei_pool_unwind_sink(wk, &sink) == 0);   /* no eval in flight */

  void *obj = NULL;
  assert(rei_pool_collect(ctrl, &t, &obj, 1000) == REI_ERR);
  assert(strcmp(rei_pool_error(ctrl), "task failed") == 0);

  exec_mode = 0;
  rei_task t2 = submit_bytes(ctrl, "ok", 2);
  assert(rei_pool_step(wk, 0) == REI_STEP_TASK);   /* healed */
  collect_bytes(ctrl, &t2, "ok", 2);
  pool_end();
  puts("ok unwind");
}

/* A retired worker lingers as the lifetime anchor for its uncollected
   results: leave does not release while rk_n > 0, the lame-duck beat
   drives the release once the last result is collected. */
static void test_retire_lame_duck(void) {
  pool_pair(1, 1, 8, 64, 64, 64, 512);
  size_t n = 100 * 1024;
  unsigned char *big = malloc(n);
  for (size_t i = 0; i < n; i++) big[i] = (unsigned char) (i * 17u);
  rei_task t = submit_bytes(ctrl, big, n);
  assert(rei_pool_step(wk, 0) == REI_STEP_TASK);   /* spilled result, kept */

  assert(rei_pool_retire(ctrl, 0) == REI_OK);
  assert(rei_pool_step(wk, 0) == REI_STEP_RETIRED);
  assert(rei_pool_leave(wk) == REI_OK);   /* rk_n > 0: no release yet */
  assert(rei_pool_lame_duck(wk) == 0);    /* still anchoring */
  collect_bytes(ctrl, &t, big, n);        /* frees the slot */
  assert(rei_pool_lame_duck(wk) == 1);    /* swept + released */
  rei_pool_destroy(wk);
  wk = NULL;
  free(big);
  pool_end();
  puts("ok retire_lame_duck");
}

/* A leaver's nonempty deque orphans (REAPING) and drains through the
   steal path; the observer of the drained deque frees the slot. */
static void test_orphan_drain(void) {
  pool_pair(2, 2, 8, 64, 64, 64, 512);
  rei_bytes b = { (void *) "q", 1 };
  rei_task t1, t2;
  assert(rei_pool_submit(wk, &b, &t1, 0) == REI_OK);   /* wk's deque */
  assert(rei_pool_submit(wk, &b, &t2, 0) == REI_OK);
  assert(rei_pool_leave(wk) == REI_OK);   /* REAPING with a queued deque */

  exec_calls = 0;
  assert(rei_pool_step(wk2, 0) == REI_STEP_TASK);   /* steals from REAPING */
  assert(rei_pool_step(wk2, 0) == REI_STEP_TASK);
  assert(exec_calls == 2);

  /* the drained slot is FREE: a fresh join claims it */
  rei_pool *wk3;
  rei_binding wb;
  make_binding(&wb, 1);
  assert(rei_pool_worker_join(&wk3, token, 0, &wb) == REI_OK);
  assert(rei_pool_leave(wk3) == REI_OK);
  rei_pool_destroy(wk3);
  rei_pool_destroy(wk);   /* released at leave (nothing kept) */
  wk = NULL;
  pool_end();
  puts("ok orphan_drain");
}

/* Submitter death, reaped in-line: a worker whose claim consumes a CANCEL
   probes the submitter — dead here — and the reap frees its slot (the
   registry of two has no room for a new attach otherwise). */
static void test_submitter_reap(void) {
  pool_pair(1, 1, 2, 64, 64, 64, 512);   /* ctrl + one more submitter */
  rei_binding b;
  make_binding(&b, 0);
  assert(rei_pool_attach(&sub, token, &b) == REI_OK);
  rei_task t = submit_bytes(sub, "doomed", 6);
  assert(rei_pool_cancel(sub, &t) == 1);
  rei_pool_destroy(sub);   /* dead with the cancelled task still queued */
  sub = NULL;

  exec_calls = 0;
  assert(rei_pool_step(wk, 0) == REI_STEP_TASK);   /* claims, skips, reaps */
  assert(exec_calls == 0);

  rei_pool *sub2;
  assert(rei_pool_attach(&sub2, token, &b) == REI_OK);   /* slot was freed */
  rei_task t2 = submit_bytes(sub2, "alive", 5);
  assert(rei_pool_step(wk, 0) == REI_STEP_TASK);
  collect_bytes(sub2, &t2, "alive", 5);
  rei_pool_destroy(sub2);
  pool_end();
  puts("ok submitter_reap");
}

/* The park path: an idle step waits out its timeout (announce -> park ->
   wake), then takes new work immediately. */
static void test_step_idle(void) {
  pool_pair(1, 1, 8, 64, 64, 64, 512);
  double t0 = rei_now();
  assert(rei_pool_step(wk, 50) == REI_STEP_IDLE);
  assert(rei_now() - t0 >= 0.04);

  rei_task t = submit_bytes(ctrl, "x", 1);
  assert(rei_pool_step(wk, 0) == REI_STEP_TASK);
  collect_bytes(ctrl, &t, "x", 1);
  pool_end();
  puts("ok step_idle");
}

/* Startup rendezvous: joined slots answer REI_OK, unjoined REI_TIMEOUT,
   out-of-range REI_ERR. */
static void test_ready_wait(void) {
  pool_pair(2, 1, 8, 64, 64, 64, 512);   /* slot 0 joined, slot 1 not */
  uint32_t slot0 = 0, slot1 = 1, slot9 = 9;
  assert(rei_pool_ready_wait(ctrl, &slot0, 1, 1000) == REI_OK);
  assert(rei_pool_ready_wait(ctrl, &slot1, 1, 30) == REI_TIMEOUT);
  assert(rei_pool_ready_wait(ctrl, &slot9, 1, 0) == REI_ERR);
  pool_end();
  puts("ok ready_wait");
}

/* The trace hook: SUBMIT on the submitter, START + DONE on the worker. */
static void test_trace(void) {
  pool_pair(1, 1, 8, 64, 64, 64, 512);
  trace_n = 0;
  assert(rei_pool_set_trace(ctrl, trace_cb, NULL) == REI_OK);
  assert(rei_pool_set_trace(wk, trace_cb, NULL) == REI_OK);
  rei_task t = submit_bytes(ctrl, "x", 1);
  assert(rei_pool_step(wk, 0) == REI_STEP_TASK);
  collect_bytes(ctrl, &t, "x", 1);
  assert(trace_n == 3);
  assert(trace_events[0] == REI_TRACE_SUBMIT);
  assert(trace_events[1] == REI_TRACE_START);
  assert(trace_events[2] == REI_TRACE_DONE);
  assert(rei_pool_set_trace(ctrl, NULL, NULL) == REI_OK);
  assert(rei_pool_set_trace(wk, NULL, NULL) == REI_OK);
  pool_end();
  puts("ok trace");
}

/* REI_TIMEOUT consumes nothing: the task stays collectible. */
static void test_collect_timeout(void) {
  pool_pair(1, 1, 8, 64, 64, 64, 512);
  rei_task t = submit_bytes(ctrl, "x", 1);
  void *obj = NULL;
  assert(rei_pool_collect(ctrl, &t, &obj, 30) == REI_TIMEOUT);
  assert(obj == NULL);
  assert(rei_pool_task_state(ctrl, &t) == REI_RS_PENDING);
  assert(rei_pool_step(wk, 0) == REI_STEP_TASK);
  collect_bytes(ctrl, &t, "x", 1);
  pool_end();
  puts("ok collect_timeout");
}

/* worker_run maps the terminal reasons without blocking when one is
   already set. */
static void test_worker_run_exits(void) {
  pool_pair(1, 1, 8, 64, 64, 64, 512);
  assert(rei_pool_retire(ctrl, 0) == REI_OK);
  assert(rei_pool_worker_run(wk) == REI_EXIT_RETIRED);
  assert(rei_pool_leave(wk) == REI_OK);
  rei_pool_destroy(wk);
  wk = NULL;
  rei_pool_destroy(ctrl);
  ctrl = NULL;

  pool_pair(1, 1, 8, 64, 64, 64, 512);
  /* the in-process worker is not running: stop's wait expires, then the
     sweep leaves the live worker alone and tears the pool down */
  assert(rei_pool_stop(ctrl, 30) == REI_TIMEOUT);
  assert(rei_pool_worker_run(wk) == REI_EXIT_SHUTDOWN);
  assert(rei_pool_leave(wk) == REI_OK);
  rei_pool_destroy(wk);
  wk = NULL;
  rei_pool_destroy(ctrl);
  ctrl = NULL;
  puts("ok worker_run_exits");
}

/* Stop over a dead worker: the wait expires (a dead worker cannot exit
   cleanly — the death-listener parcel's off-thread reap is what answers
   REI_OK there), then the teardown sweep reaps the slot. */
static void test_stop_reaps_dead_worker(void) {
  pool_pair(1, 1, 8, 64, 64, 64, 512);
  rei_pool_destroy(wk);   /* simulated crash before any leave */
  wk = NULL;
  assert(rei_pool_stop(ctrl, 30) == REI_TIMEOUT);
  rei_pool_destroy(ctrl);
  ctrl = NULL;
  puts("ok stop_reaps_dead_worker");
}

/* collect_any: the first terminal handle wins, ties break to the
   earliest position; REI_TIMEOUT consumes nothing; a cancelled task
   reports (its index is set even on the error return). */
static void test_collect_any(void) {
  pool_pair(1, 1, 8, 64, 64, 64, 512);
  rei_task t1 = submit_bytes(ctrl, "a", 1);
  rei_task t2 = submit_bytes(ctrl, "b", 1);
  rei_task t3 = submit_bytes(ctrl, "c", 1);
  rei_task ts[3] = { t1, t2, t3 };

  size_t idx = 99;
  void *obj = NULL;
  assert(rei_pool_collect_any(ctrl, ts, 3, &idx, &obj, 0) == REI_TIMEOUT);
  assert(obj == NULL);
  assert(rei_pool_task_state(ctrl, &t1) == REI_RS_PENDING);   /* unconsumed */

  assert(rei_pool_step(wk, 0) == REI_STEP_TASK);
  assert(rei_pool_step(wk, 0) == REI_STEP_TASK);   /* two terminal: a tie */
  assert(rei_pool_collect_any(ctrl, ts, 3, &idx, &obj, 1000) == REI_OK);
  assert(idx == 0);
  rei_bytes *b = obj;
  assert(b->len == 1 && memcmp(b->data, "a", 1) == 0);
  rei_bytes_free(b);

  assert(rei_pool_cancel(ctrl, &t3) == 1);
  rei_task ts2[2] = { t2, t3 };
  obj = NULL;
  assert(rei_pool_collect_any(ctrl, ts2, 2, &idx, &obj, 1000) == REI_OK);
  assert(idx == 0);   /* both terminal: the earliest position */
  rei_bytes_free(obj);
  obj = NULL;
  assert(rei_pool_collect_any(ctrl, &t3, 1, &idx, &obj, 1000) == REI_ERR);
  assert(idx == 0);
  assert(strcmp(rei_pool_error(ctrl), "task cancelled or pool stopped") == 0);
  pool_end();
  puts("ok collect_any");
}

/* collect_all: fills in input order, stopping at the first non-OK by
   position inclusive; handles past it stay collectible; REI_TIMEOUT
   consumes nothing; *err_index_out == n means all OK. */
static void test_collect_all(void) {
  pool_pair(1, 1, 8, 64, 64, 64, 512);
  rei_task t1 = submit_bytes(ctrl, "a", 1);
  rei_task t2 = submit_bytes(ctrl, "err", 3);
  rei_task t3 = submit_bytes(ctrl, "c", 1);
  rei_task ts[3] = { t1, t2, t3 };
  void *vals[3] = { (void *) 1, (void *) 1, (void *) 1 };
  size_t err = 99;
  assert(rei_pool_collect_all(ctrl, ts, 3, vals, &err, 0) == REI_TIMEOUT);
  assert(vals[0] == NULL && vals[1] == NULL && vals[2] == NULL);

  assert(rei_pool_step(wk, 0) == REI_STEP_TASK);
  assert(rei_pool_step(wk, 0) == REI_STEP_TASK);
  assert(rei_pool_step(wk, 0) == REI_STEP_TASK);
  err_read = 1;   /* read the ERR envelope's bytes as the value */
  assert(rei_pool_collect_all(ctrl, ts, 3, vals, &err, 1000) == REI_OK);
  err_read = 0;
  assert(err == 1);
  rei_bytes *b = vals[0];
  assert(b->len == 1 && memcmp(b->data, "a", 1) == 0);
  rei_bytes_free(b);
  b = vals[1];
  assert(b->len == 5 && memcmp(b->data, "boom", 4) == 0);
  rei_bytes_free(b);
  assert(vals[2] == NULL);
  collect_bytes(ctrl, &t3, "c", 1);   /* past the error: still collectible */

  rei_task t4 = submit_bytes(ctrl, "d", 1);
  assert(rei_pool_step(wk, 0) == REI_STEP_TASK);
  err = 0;
  assert(rei_pool_collect_all(ctrl, &t4, 1, vals, &err, 1000) == REI_OK);
  assert(err == 1);   /* == n: all OK */
  rei_bytes_free(vals[0]);
  pool_end();
  puts("ok collect_all");
}

/* status/dump/tasks snapshots: wire-state reads plus the handle-local
   counters. */
static void test_introspection(void) {
  pool_pair(1, 1, 8, 64, 64, 64, 512);
  rei_task t1 = submit_bytes(ctrl, "a", 1);
  rei_task t2 = submit_bytes(ctrl, "b", 1);
  assert(rei_pool_step(wk, 0) == REI_STEP_TASK);   /* t1 OK, uncollected */

  rei_pool_status st;
  assert(rei_pool_status_get(ctrl, &st) == REI_OK);
  assert(st.size == sizeof(st));
  assert(st.role == REI_ROLE_CONTROLLER);
  assert(st.max_workers == 1 && st.max_submitters == 8);
  assert(st.n_workers == 1 && st.n_submitters == 1);
  assert(st.worker_state[0] == REI_WK_LIVE);
  assert(st.sub_state[0] == REI_SUB_LIVE);
  assert(st.sub_state[1] == REI_SUB_FREE);
  assert(st.tasks_by_state[REI_RS_OK] == 1);
  assert(st.tasks_by_state[REI_RS_PENDING] == 1);
  assert(st.inj_queued[0] == 1);   /* t2 unclaimed */
  assert(st.shutdown == 0);

  rei_pool_dump d;
  assert(rei_pool_dump_get(ctrl, &d) == REI_OK);
  assert(d.size == sizeof(d));
  assert(d.n_workers == 1 && d.n_submitters == 8);
  assert(d.workers[0].pid == (int64_t) rei_self_pid());
  assert(d.workers[0].status == REI_WK_LIVE);
  assert(d.submitters[0].injected == 2);
  assert(d.submitters[0].claimed == 1);
  assert(d.submitters[0].rs_count == 8);
  assert(d.help_wanted == 1);   /* no running worker to wake */
  assert(d.status.tasks_by_state[REI_RS_OK] == 1);

  rei_rs_row rows[8];
  uint32_t nrows = 0;
  assert(rei_pool_tasks_get(ctrl, rows, 8, &nrows) == REI_OK);
  assert(nrows == 2);
  assert(rows[0].slot == t1.rs_index && rows[0].status == REI_RS_OK);
  assert(rows[0].sequence == t1.seq);
  assert(rows[1].slot == t2.rs_index && rows[1].status == REI_RS_PENDING);
  uint32_t nrows2 = 0;
  assert(rei_pool_tasks_get(ctrl, rows, 1, &nrows2) == REI_OK);
  assert(nrows2 == 2);   /* the total, not the fill count */

  collect_bytes(ctrl, &t1, "a", 1);
  assert(rei_pool_step(wk, 0) == REI_STEP_TASK);
  collect_bytes(ctrl, &t2, "b", 1);
  pool_end();
  puts("ok introspection");
}

/* Map support: the signal trio, map_caps, a RUNNER-flagged submit
   re-homed by the doorbell help beat, an ordinary claim executed inline
   by it, and the test-harness deque pull. */
static void test_map_support(void) {
  pool_pair(1, 1, 8, 64, 64, 64, 512);
  uint32_t free_rs = 0, inj_cap = 0, inline_entry = 0;
  assert(rei_pool_map_caps(ctrl, &free_rs, &inj_cap, &inline_entry) == 0);
  assert(free_rs == 8 && inj_cap == 64);
  assert(inline_entry == 512 - (uint32_t) sizeof(rei_entry_hdr));

  rei_pool_sig *sig = rei_pool_signals(ctrl);
  assert(sig != NULL);
  assert(*sig->shutdown == 0 && *sig->owner_dead == 0);

  /* a RUNNER submit rings the doorbell (no parked worker to wake) */
  rei_bytes b = { (void *) "r", 1 };
  rei_task tr;
  assert(rei_pool_submit_flags(ctrl, &b, REI_ENTRY_RUNNER, &tr, 0) == REI_OK);
  assert(*sig->help_wanted == 1);

  /* the help beat re-homes the runner onto the worker's own deque
     instead of executing it */
  exec_calls = 0;
  assert(rei_pool_help_once(wk) == 1);
  assert(exec_calls == 0);
  assert(*sig->help_wanted == 0);   /* ring drained: the bell stays clear */
  rei_pool_status st;
  assert(rei_pool_status_get(ctrl, &st) == REI_OK);
  assert(st.deque_depth[0] == 1);
  assert(rei_pool_help_once(wk) == 0);   /* nothing left to claim */
  assert(rei_pool_step(wk, 0) == REI_STEP_TASK);   /* pops its own deque */
  collect_bytes(ctrl, &tr, "r", 1);

  /* an ordinary claim executes inline under the help machinery */
  exec_calls = 0;
  rei_task t0 = submit_bytes(ctrl, "z", 1);
  assert(rei_pool_help_once(wk) == 1);
  assert(exec_calls == 1);
  collect_bytes(ctrl, &t0, "z", 1);

  /* the deque pull queues ring entries without executing them */
  rei_task t1 = submit_bytes(ctrl, "x", 1);
  rei_task t2 = submit_bytes(ctrl, "y", 1);
  assert(rei_pool_deque_pull(wk, 2) == 2);
  assert(rei_pool_status_get(ctrl, &st) == REI_OK);
  assert(st.deque_depth[0] == 2 && st.inj_queued[0] == 0);
  assert(rei_pool_step(wk, 0) == REI_STEP_TASK);
  assert(rei_pool_step(wk, 0) == REI_STEP_TASK);
  collect_bytes(ctrl, &t1, "x", 1);
  collect_bytes(ctrl, &t2, "y", 1);

  /* a worker's first map_caps claims its nested-submitter slot */
  assert(rei_pool_map_caps(wk, &free_rs, NULL, NULL) == 0);
  assert(free_rs == 8);
  free(sig);
  pool_end();
  puts("ok map_support");
}

int main(void) {
  rei_binding_bytes(&bytesb);

  test_round_trip();
  test_spill();
  test_batch();
  test_exhaustion();
  test_cancel();
  test_attach();
  test_stop();
  test_steal();
  test_nested();
  test_err_publish();
  test_infra_failure_and_reap();
  test_unwind();
  test_retire_lame_duck();
  test_orphan_drain();
  test_submitter_reap();
  test_step_idle();
  test_ready_wait();
  test_trace();
  test_collect_timeout();
  test_worker_run_exits();
  test_stop_reaps_dead_worker();
  test_collect_any();
  test_collect_all();
  test_introspection();
  test_map_support();

  puts("test_pool: all passed");
  return 0;
}
