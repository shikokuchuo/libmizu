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

static mizu_binding bytesb;
static int exec_mode = 0;  /* 0 echo, 1 strand (no publish), 2 infra fail,
                              3 abandon (eval_mark + longjmp), 4 ERR inline,
                              5 ERR tiered */
static int err_read = 0;   /* read_fn returns the ERR envelope's bytes */

static void array_sink(void *ctx, size_t i, void *obj) {
  ((void **) ctx)[i] = obj;
}
static int exec_calls = 0;
static jmp_buf abandon_jmp;

/* Resolve a task frame's bytes: INLINE/NIL directly, SHM_RAW through the
   read-side region service (the worker handle's open cache) on the exec
   ctx. */
static void task_bytes(mizu_read_ctx *ctx, const mizu_slot_hdr *hdr,
                       const uint8_t *payload, size_t limit,
                       const uint8_t **bytes, size_t *n) {
  switch (hdr->kind) {
  case MIZU_KIND_NIL:
    *bytes = NULL;
    *n = 0;
    return;
  case MIZU_KIND_INLINE:
    assert(hdr->len <= limit);
    *bytes = payload;
    *n = hdr->len;
    return;
  case MIZU_KIND_SHM_RAW: {
    mizu_shm *shm = mizu_read_region(ctx, payload, hdr->len);
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

static int test_exec(const mizu_slot_hdr *hdr, const uint8_t *payload,
                     size_t limit, mizu_result_sink *sink, int catching,
                     mizu_read_ctx *ctx) {
  /* payload-keyed outcome, ahead of the mode dispatch: "err" publishes
     the INLINE-framed ERR envelope */
  if (hdr->kind == MIZU_KIND_INLINE && hdr->len == 3 &&
      memcmp(payload, "err", 3) == 0) {
    static const char msg[] = "boom";
    assert(sizeof(msg) <= sink->inline_max);
    memcpy(sink->payload, msg, sizeof(msg));
    assert(mizu_result_publish_err(sink, NULL, (uint32_t) sizeof(msg)) == 1);
    return 0;
  }
  switch (exec_mode) {
  case 1:
    return 0;   /* claim, never publish: strands the in-flight task */
  case 2:
    return 1;   /* infrastructure failure: takes the worker down */
  case 3:
    if (!catching) {
      mizu_pool_eval_mark(sink->p, 1);
      longjmp(abandon_jmp, 1);   /* the binding's unwind path (R's longjmp) */
    }
    break;
  case 4: {
    /* the ERR envelope framed INLINE in the sink's frame buffer */
    static const char msg[] = "boom";
    assert(sizeof(msg) <= sink->inline_max);
    memcpy(sink->payload, msg, sizeof(msg));
    assert(mizu_result_publish_err(sink, NULL, (uint32_t) sizeof(msg)) == 1);
    return 0;
  }
  case 5: {
    /* the ERR envelope riding the tiered stage */
    mizu_bytes b = { (void *) "boom-tiered", 11 };
    assert(mizu_result_publish_err(sink, &b, 0) == 1);
    return 0;
  }
  default:
    break;
  }
  /* echo: the task bytes come back as the result */
  const uint8_t *bytes = NULL;   /* MinGW's assert is not noreturn, so */
  size_t n = 0;                  /* task_bytes' default case warns */
  task_bytes(ctx, hdr, payload, limit, &bytes, &n);
  mizu_bytes b = { (void *) bytes, n };
  assert(mizu_result_publish(sink, &b) == 1);
  exec_calls++;
  return 0;
}

static void *test_read(const mizu_slot_hdr *hdr, const uint8_t *payload,
                       size_t limit, mizu_read_ctx *ctx) {
  if (err_read && ctx->outcome == MIZU_RS_ERR) {
    /* read the envelope's bytes as if OK: verifies the framing crossed */
    mizu_read_ctx tmp = *ctx;
    tmp.outcome = MIZU_RS_OK;
    return bytesb.read(hdr, payload, limit, &tmp);
  }
  return bytesb.read(hdr, payload, limit, ctx);
}

static int trace_events[16];
static int trace_n;
static void trace_cb(mizu_trace_event ev, uint64_t task_id, void *ctx) {
  (void) task_id;
  (void) ctx;
  if (trace_n < 16) trace_events[trace_n++] = (int) ev;
}

// Harness ------------------------------------------------------------------------------

static mizu_pool *ctrl, *wk, *wk2, *sub;
static char token[64];

static void make_binding(mizu_binding *b, int worker) {
  mizu_binding_bytes(b);
  b->read = test_read;
  if (worker) b->exec = test_exec;
}

/* Controller plus `joined` worker handles (of `workers` slots), all
   in-process. */
static void pool_pair(uint32_t workers, uint32_t joined, uint32_t max_sub,
                      uint32_t inj, uint32_t dq, uint32_t rs, uint32_t slot) {
  mizu_pool_opts opts;
  mizu_pool_opts_init(&opts);
  opts.max_workers = workers;
  opts.max_submitters = max_sub;
  opts.injection_cap = inj;
  opts.per_worker_cap = dq;
  opts.result_slots = rs;
  opts.slot_size = slot;
  mizu_binding b, wb;
  make_binding(&b, 0);
  make_binding(&wb, 1);
  assert(mizu_pool_create(&ctrl, &opts, &b) == MIZU_OK);
  assert(mizu_pool_token(ctrl, token, sizeof(token)) == MIZU_OK);
  assert(joined >= 1 && joined <= workers && joined <= 2);
  assert(mizu_pool_worker_join(&wk, token, 0, &wb) == MIZU_OK);
  wk2 = NULL;
  if (joined > 1)
    assert(mizu_pool_worker_join(&wk2, token, 1, &wb) == MIZU_OK);
  sub = NULL;
}

static void pool_end(void) {
  if (sub != NULL) {
    mizu_pool_destroy(sub);
    sub = NULL;
  }
  if (wk2 != NULL) {
    assert(mizu_pool_leave(wk2) == MIZU_OK);
    mizu_pool_destroy(wk2);
    wk2 = NULL;
  }
  if (wk != NULL) {
    assert(mizu_pool_leave(wk) == MIZU_OK);
    mizu_pool_destroy(wk);
    wk = NULL;
  }
  mizu_pool_destroy(ctrl);
  ctrl = NULL;
}

static mizu_task submit_bytes(mizu_pool *p, const void *data, size_t len) {
  mizu_bytes b = { (void *) data, len };
  mizu_task t;
  assert(mizu_pool_submit(p, &b, &t, 1000) == MIZU_OK);
  return t;
}

static void collect_bytes(mizu_pool *p, mizu_task *t, const void *expect,
                          size_t len) {
  void *obj = NULL;
  assert(mizu_pool_collect(p, t, &obj, 1000) == MIZU_OK);
  mizu_bytes *b = obj;
  assert(b->len == len);
  assert(len == 0 || memcmp(b->data, expect, len) == 0);
  mizu_bytes_free(b);
}

// Scenarios ------------------------------------------------------------------------------

/* Round trip: INLINE and NIL tasks, submit -> step -> collect. */
static void test_round_trip(void) {
  pool_pair(1, 1, 8, 64, 64, 64, 512);
  mizu_task t = submit_bytes(ctrl, "hello", 5);
  assert(mizu_pool_task_state(ctrl, &t) == MIZU_RS_PENDING);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);
  assert(mizu_pool_task_state(ctrl, &t) == MIZU_RS_OK);
  collect_bytes(ctrl, &t, "hello", 5);
  assert(mizu_pool_task_state(ctrl, &t) == MIZU_RS_FREE);   /* collected */

  mizu_task t2 = submit_bytes(ctrl, NULL, 0);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);
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

  mizu_task t = submit_bytes(ctrl, big, n);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);
  collect_bytes(ctrl, &t, big, n);

  uint64_t hits_before = ((mizu_handle *) ctrl)->fl.hits;
  mizu_task t2 = submit_bytes(ctrl, big, n);
  assert(((mizu_handle *) ctrl)->fl.hits > hits_before);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);
  collect_bytes(ctrl, &t2, big, n);
  free(big);
  pool_end();
  puts("ok spill");
}

/* Batches: a burst larger than the ring ends early at the deadline
   (MIZU_OK, *n_out < n); the single form answers MIZU_FULL. */
static void test_batch(void) {
  pool_pair(1, 1, 8, 2, 64, 64, 512);   /* injection_cap = 2 */
  mizu_bytes objs[4] = { { (void *) "a", 1 }, { (void *) "b", 1 },
                        { (void *) "c", 1 }, { (void *) "d", 1 } };
  void *objs_p[4] = { &objs[0], &objs[1], &objs[2], &objs[3] };
  mizu_task ts[4];
  size_t n_out = 99;
  assert(mizu_pool_submit_batch(ctrl, objs_p, 4, ts, &n_out, 0) == MIZU_OK);
  assert(n_out == 2);

  mizu_bytes e = { (void *) "e", 1 };
  mizu_task te;
  assert(mizu_pool_submit(ctrl, &e, &te, 0) == MIZU_FULL);

  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);
  collect_bytes(ctrl, &ts[0], "a", 1);
  collect_bytes(ctrl, &ts[1], "b", 1);

  assert(mizu_pool_submit(ctrl, &e, &te, 1000) == MIZU_OK);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);
  collect_bytes(ctrl, &te, "e", 1);
  pool_end();
  puts("ok batch");
}

/* Result-slot exhaustion raises (MIZU_ERRCAT_EXHAUSTED), distinct from a
   full ring (MIZU_FULL). */
static void test_exhaustion(void) {
  pool_pair(1, 1, 2, 64, 64, 4, 512);   /* 2 submitters, 2 slots each */
  mizu_task a = submit_bytes(ctrl, "a", 1);
  mizu_task b = submit_bytes(ctrl, "b", 1);
  mizu_bytes c = { (void *) "c", 1 };
  mizu_task tc;
  assert(mizu_pool_submit(ctrl, &c, &tc, 0) == MIZU_ERR);
  assert(mizu_pool_errcat(ctrl) == MIZU_ERRCAT_EXHAUSTED);

  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);
  collect_bytes(ctrl, &a, "a", 1);
  assert(mizu_pool_submit(ctrl, &c, &tc, 1000) == MIZU_OK);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);   /* b */
  collect_bytes(ctrl, &b, "b", 1);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);   /* c */
  collect_bytes(ctrl, &tc, "c", 1);
  pool_end();
  puts("ok exhaustion");
}

/* Cancel: advisory, discard-only. A cancelled task collects as MIZU_ERR
   (the bytes binding builds no error object); the worker's claim skips it
   without executing. */
static void test_cancel(void) {
  pool_pair(1, 1, 8, 64, 64, 64, 512);
  mizu_task t = submit_bytes(ctrl, "x", 1);
  assert(mizu_pool_cancel(ctrl, &t) == 1);
  assert(mizu_pool_task_state(ctrl, &t) == MIZU_RS_CANCEL);
  assert(mizu_pool_cancel(ctrl, &t) == 0);   /* already cancelled */

  void *obj = NULL;
  assert(mizu_pool_collect(ctrl, &t, &obj, 1000) == MIZU_ERR);
  assert(obj == NULL);
  assert(strcmp(mizu_pool_error(ctrl), "task cancelled or pool stopped") == 0);

  exec_calls = 0;
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);   /* claims, skips */
  assert(exec_calls == 0);

  mizu_task t2 = submit_bytes(ctrl, "y", 1);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);
  assert(mizu_pool_cancel(ctrl, &t2) == 0);   /* too late: completed */
  assert(mizu_pool_task_state(ctrl, &t2) == MIZU_RS_OK);  /* left collectible */
  collect_bytes(ctrl, &t2, "y", 1);

  /* the finalizer release: a terminal, uncollected slot is freed (0 —
     released, not cancelled) so the result never lingers until reuse */
  mizu_task t3 = submit_bytes(ctrl, "z", 1);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);
  assert(mizu_pool_task_state(ctrl, &t3) == MIZU_RS_OK);
  assert(mizu_pool_task_release(ctrl, &t3) == 0);
  assert(mizu_pool_task_state(ctrl, &t3) == MIZU_RS_FREE);
  /* a pending task is cancelled through the same verb */
  mizu_task t4 = submit_bytes(ctrl, "w", 1);
  assert(mizu_pool_task_release(ctrl, &t4) == 1);
  assert(mizu_pool_task_state(ctrl, &t4) == MIZU_RS_CANCEL);
  pool_end();
  puts("ok cancel");
}

/* A second submitter joins by token; malformed and unknown tokens are
   refused. */
static void test_attach(void) {
  pool_pair(1, 1, 8, 64, 64, 64, 512);
  mizu_binding b;
  make_binding(&b, 0);
  assert(mizu_pool_attach(&sub, token, &b) == MIZU_OK);
  mizu_task t = submit_bytes(sub, "from-sub", 8);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);
  collect_bytes(sub, &t, "from-sub", 8);

  mizu_pool *bad;
  assert(mizu_pool_attach(&bad, "nonsense!", &b) == MIZU_ERR);
  assert(mizu_pool_attach(&bad, "deadbeef_deadbeef", &b) == MIZU_ERR);
  pool_end();
  puts("ok attach");
}

/* Stop: cancels pending tasks (a parked collect would wake to CANCEL),
   refuses new submits and late attaches, is idempotent. */
static void test_stop(void) {
  pool_pair(1, 1, 8, 64, 64, 64, 512);
  mizu_binding b;
  make_binding(&b, 0);
  assert(mizu_pool_attach(&sub, token, &b) == MIZU_OK);
  mizu_task t = submit_bytes(sub, "doomed", 6);   /* never run */

  assert(mizu_pool_leave(wk) == MIZU_OK);   /* the worker exits cleanly first */
  assert(mizu_pool_stop(ctrl, 1000) == MIZU_OK);

  void *obj = NULL;
  assert(mizu_pool_collect(sub, &t, &obj, 1000) == MIZU_ERR);
  assert(strcmp(mizu_pool_error(sub), "task cancelled or pool stopped") == 0);

  mizu_bytes z = { (void *) "z", 1 };
  mizu_task tz;
  assert(mizu_pool_submit(sub, &z, &tz, 0) == MIZU_ERR);
  assert(mizu_pool_errcat(sub) == MIZU_ERRCAT_STOPPED);

  mizu_pool *late;
  assert(mizu_pool_attach(&late, token, &b) == MIZU_ERR);

  assert(mizu_pool_stop(ctrl, 0) == MIZU_OK);   /* no-op */
  mizu_pool_destroy(sub);
  sub = NULL;
  mizu_pool_destroy(wk);
  wk = NULL;
  mizu_pool_destroy(ctrl);
  ctrl = NULL;
  puts("ok stop");
}

/* Steal: a nested submit queues on the worker's own deque; a peer's step
   steals and executes it. */
static void test_steal(void) {
  pool_pair(2, 2, 8, 64, 64, 64, 512);
  mizu_bytes b = { (void *) "nested", 6 };
  mizu_task t;
  assert(mizu_pool_submit(wk, &b, &t, 0) == MIZU_OK);   /* onto wk's deque */
  exec_calls = 0;
  assert(mizu_pool_step(wk2, 0) == MIZU_STEP_TASK);     /* stolen by wk2 */
  assert(exec_calls == 1);
  collect_bytes(wk, &t, "nested", 6);   /* nested tasks collect on wk */
  pool_end();
  puts("ok steal");
}

/* Nested submit with a full deque runs the task inline; a nested collect
   helps (executes its own queued work) instead of parking. */
static void test_nested(void) {
  pool_pair(1, 1, 8, 64, 2, 64, 512);   /* per_worker_cap = 2 */
  mizu_bytes b = { (void *) "n", 1 };
  mizu_task t1, t2, t3;
  assert(mizu_pool_submit(wk, &b, &t1, 0) == MIZU_OK);   /* deque: 1 */
  assert(mizu_pool_submit(wk, &b, &t2, 0) == MIZU_OK);   /* deque: 2 (full) */
  exec_calls = 0;
  assert(mizu_pool_submit(wk, &b, &t3, 0) == MIZU_OK);   /* inline execute */
  assert(exec_calls == 1);
  collect_bytes(wk, &t3, "n", 1);   /* already published */
  collect_bytes(wk, &t1, "n", 1);   /* help pops + runs both queued */
  collect_bytes(wk, &t2, "n", 1);
  assert(exec_calls == 3);
  pool_end();
  puts("ok nested");
}

/* The ERR publish: the envelope crosses INLINE (and tiered), the binding
   reads it as an error object; the bytes binding surfaces MIZU_ERR. */
static void test_err_publish(void) {
  pool_pair(1, 1, 8, 64, 64, 64, 512);
  void *obj = NULL;

  exec_mode = 4;
  mizu_task t = submit_bytes(ctrl, "x", 1);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);
  assert(mizu_pool_task_state(ctrl, &t) == MIZU_RS_ERR);
  err_read = 1;
  assert(mizu_pool_collect(ctrl, &t, &obj, 1000) == MIZU_OK);
  mizu_bytes *eb = obj;
  assert(eb->len == 5 && memcmp(eb->data, "boom", 4) == 0);
  mizu_bytes_free(eb);
  err_read = 0;

  exec_mode = 5;
  mizu_task t2 = submit_bytes(ctrl, "x", 1);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);
  err_read = 1;
  assert(mizu_pool_collect(ctrl, &t2, &obj, 1000) == MIZU_OK);
  eb = obj;
  assert(eb->len == 11 && memcmp(eb->data, "boom-tiered", 11) == 0);
  mizu_bytes_free(eb);
  err_read = 0;

  exec_mode = 4;
  mizu_task t3 = submit_bytes(ctrl, "x", 1);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);
  assert(mizu_pool_collect(ctrl, &t3, &obj, 1000) == MIZU_ERR);
  assert(strcmp(mizu_pool_error(ctrl), "task failed") == 0);
  exec_mode = 0;
  pool_end();
  puts("ok err_publish");
}

/* An exec_fn infrastructure failure takes the worker down
   (MIZU_STEP_SHUTDOWN, the error recorded); the stranded in-flight task
   fails DIED once the reaper confirms the worker's death — here via the
   collect wake-backstop probe after a simulated crash. */
static void test_infra_failure_and_reap(void) {
  pool_pair(1, 1, 8, 64, 64, 64, 512);
  exec_mode = 2;
  mizu_task t = submit_bytes(ctrl, "x", 1);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_SHUTDOWN);
  assert(mizu_pool_errcat(wk) == MIZU_ERRCAT_OTHER);

  mizu_pool_destroy(wk);   /* simulated crash: lock releases, slot LIVE */
  wk = NULL;
  void *obj = NULL;
  assert(mizu_pool_collect(ctrl, &t, &obj, 500) == MIZU_ERR);
  assert(strcmp(mizu_pool_error(ctrl),
                "worker died while executing this task") == 0);
  exec_mode = 0;
  pool_end();
  puts("ok infra_failure_and_reap");
}

/* The unwind path: an abandoned catching = 0 eval (a binding's longjmp)
   mints the in-flight task's sink through mizu_pool_unwind_sink for the
   ERR publish; the worker loop heals and continues. */
static void test_unwind(void) {
  pool_pair(1, 1, 8, 64, 64, 64, 512);
  exec_mode = 3;
  mizu_task t = submit_bytes(ctrl, "x", 1);
  if (setjmp(abandon_jmp) == 0) {
    mizu_pool_step(wk, 0);
    assert(0);   /* the exec abandons: step never returns */
  }
  mizu_result_sink sink;
  assert(mizu_pool_unwind_sink(wk, &sink) == 1);
  static const char emsg[] = "eval blew up";
  assert(sizeof(emsg) <= sink.inline_max);
  memcpy(sink.payload, emsg, sizeof(emsg));
  assert(mizu_result_publish_err(&sink, NULL, (uint32_t) sizeof(emsg)) == 1);
  assert(mizu_pool_unwind_sink(wk, &sink) == 0);   /* no eval in flight */

  void *obj = NULL;
  assert(mizu_pool_collect(ctrl, &t, &obj, 1000) == MIZU_ERR);
  assert(strcmp(mizu_pool_error(ctrl), "task failed") == 0);

  exec_mode = 0;
  mizu_task t2 = submit_bytes(ctrl, "ok", 2);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);   /* healed */
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
  mizu_task t = submit_bytes(ctrl, big, n);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);   /* spilled result, kept */

  assert(mizu_pool_retire(ctrl, 0) == MIZU_OK);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_RETIRED);
  assert(mizu_pool_leave(wk) == MIZU_OK);   /* rk_n > 0: no release yet */
  assert(mizu_pool_lame_duck(wk) == 0);    /* still anchoring */
  collect_bytes(ctrl, &t, big, n);        /* frees the slot */
  assert(mizu_pool_lame_duck(wk) == 1);    /* swept + released */
  mizu_pool_destroy(wk);
  wk = NULL;
  free(big);
  pool_end();
  puts("ok retire_lame_duck");
}

/* A leaver's nonempty deque orphans (REAPING) and drains through the
   steal path; the observer of the drained deque frees the slot. */
static void test_orphan_drain(void) {
  pool_pair(2, 2, 8, 64, 64, 64, 512);
  mizu_bytes b = { (void *) "q", 1 };
  mizu_task t1, t2;
  assert(mizu_pool_submit(wk, &b, &t1, 0) == MIZU_OK);   /* wk's deque */
  assert(mizu_pool_submit(wk, &b, &t2, 0) == MIZU_OK);
  assert(mizu_pool_leave(wk) == MIZU_OK);   /* REAPING with a queued deque */

  exec_calls = 0;
  assert(mizu_pool_step(wk2, 0) == MIZU_STEP_TASK);   /* steals from REAPING */
  assert(mizu_pool_step(wk2, 0) == MIZU_STEP_TASK);
  assert(exec_calls == 2);

  /* the drained slot is FREE: a fresh join claims it */
  mizu_pool *wk3;
  mizu_binding wb;
  make_binding(&wb, 1);
  assert(mizu_pool_worker_join(&wk3, token, 0, &wb) == MIZU_OK);
  assert(mizu_pool_leave(wk3) == MIZU_OK);
  mizu_pool_destroy(wk3);
  mizu_pool_destroy(wk);   /* released at leave (nothing kept) */
  wk = NULL;
  pool_end();
  puts("ok orphan_drain");
}

/* Submitter death, reaped in-line: a worker whose claim consumes a CANCEL
   probes the submitter — dead here — and the reap frees its slot (the
   registry of two has no room for a new attach otherwise). */
static void test_submitter_reap(void) {
  pool_pair(1, 1, 2, 64, 64, 64, 512);   /* ctrl + one more submitter */
  mizu_binding b;
  make_binding(&b, 0);
  assert(mizu_pool_attach(&sub, token, &b) == MIZU_OK);
  mizu_task t = submit_bytes(sub, "doomed", 6);
  assert(mizu_pool_cancel(sub, &t) == 1);
  mizu_pool_destroy(sub);   /* dead with the cancelled task still queued */
  sub = NULL;

  exec_calls = 0;
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);   /* claims, skips, reaps */
  assert(exec_calls == 0);

  mizu_pool *sub2;
  assert(mizu_pool_attach(&sub2, token, &b) == MIZU_OK);   /* slot was freed */
  mizu_task t2 = submit_bytes(sub2, "alive", 5);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);
  collect_bytes(sub2, &t2, "alive", 5);
  mizu_pool_destroy(sub2);
  pool_end();
  puts("ok submitter_reap");
}

/* The park path: an idle step waits out its timeout (announce -> park ->
   wake), then takes new work immediately. */
static void test_step_idle(void) {
  pool_pair(1, 1, 8, 64, 64, 64, 512);
  double t0 = mizu_now();
  assert(mizu_pool_step(wk, 50) == MIZU_STEP_IDLE);
  assert(mizu_now() - t0 >= 0.04);

  mizu_task t = submit_bytes(ctrl, "x", 1);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);
  collect_bytes(ctrl, &t, "x", 1);
  pool_end();
  puts("ok step_idle");
}

/* Startup rendezvous: joined slots answer MIZU_OK, unjoined MIZU_TIMEOUT,
   out-of-range MIZU_ERR. */
static void test_ready_wait(void) {
  pool_pair(2, 1, 8, 64, 64, 64, 512);   /* slot 0 joined, slot 1 not */
  uint32_t slot0 = 0, slot1 = 1, slot9 = 9;
  assert(mizu_pool_ready_wait(ctrl, &slot0, 1, 1000) == MIZU_OK);
  assert(mizu_pool_ready_wait(ctrl, &slot1, 1, 30) == MIZU_TIMEOUT);
  assert(mizu_pool_ready_wait(ctrl, &slot9, 1, 0) == MIZU_ERR);
  pool_end();
  puts("ok ready_wait");
}

/* The trace hook: SUBMIT on the submitter, START + DONE on the worker. */
static void test_trace(void) {
  pool_pair(1, 1, 8, 64, 64, 64, 512);
  trace_n = 0;
  assert(mizu_pool_set_trace(ctrl, trace_cb, NULL) == MIZU_OK);
  assert(mizu_pool_set_trace(wk, trace_cb, NULL) == MIZU_OK);
  mizu_task t = submit_bytes(ctrl, "x", 1);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);
  collect_bytes(ctrl, &t, "x", 1);
  assert(trace_n == 3);
  assert(trace_events[0] == MIZU_TRACE_SUBMIT);
  assert(trace_events[1] == MIZU_TRACE_START);
  assert(trace_events[2] == MIZU_TRACE_DONE);
  assert(mizu_pool_set_trace(ctrl, NULL, NULL) == MIZU_OK);
  assert(mizu_pool_set_trace(wk, NULL, NULL) == MIZU_OK);
  pool_end();
  puts("ok trace");
}

/* MIZU_TIMEOUT consumes nothing: the task stays collectible. */
static void test_collect_timeout(void) {
  pool_pair(1, 1, 8, 64, 64, 64, 512);
  mizu_task t = submit_bytes(ctrl, "x", 1);
  void *obj = NULL;
  assert(mizu_pool_collect(ctrl, &t, &obj, 30) == MIZU_TIMEOUT);
  assert(obj == NULL);
  assert(mizu_pool_task_state(ctrl, &t) == MIZU_RS_PENDING);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);
  collect_bytes(ctrl, &t, "x", 1);
  pool_end();
  puts("ok collect_timeout");
}

/* worker_run maps the terminal reasons without blocking when one is
   already set. */
static void test_worker_run_exits(void) {
  pool_pair(1, 1, 8, 64, 64, 64, 512);
  assert(mizu_pool_retire(ctrl, 0) == MIZU_OK);
  assert(mizu_pool_worker_run(wk) == MIZU_EXIT_RETIRED);
  assert(mizu_pool_leave(wk) == MIZU_OK);
  mizu_pool_destroy(wk);
  wk = NULL;
  mizu_pool_destroy(ctrl);
  ctrl = NULL;

  pool_pair(1, 1, 8, 64, 64, 64, 512);
  /* the in-process worker is not running: stop's wait expires, then the
     sweep leaves the live worker alone and tears the pool down */
  assert(mizu_pool_stop(ctrl, 30) == MIZU_TIMEOUT);
  assert(mizu_pool_worker_run(wk) == MIZU_EXIT_SHUTDOWN);
  assert(mizu_pool_leave(wk) == MIZU_OK);
  mizu_pool_destroy(wk);
  wk = NULL;
  mizu_pool_destroy(ctrl);
  ctrl = NULL;
  puts("ok worker_run_exits");
}

/* Stop over a dead worker: the wait expires (a dead worker cannot exit
   cleanly — the death-listener parcel's off-thread reap is what answers
   MIZU_OK there), then the teardown sweep reaps the slot. */
static void test_stop_reaps_dead_worker(void) {
  pool_pair(1, 1, 8, 64, 64, 64, 512);
  mizu_pool_destroy(wk);   /* simulated crash before any leave */
  wk = NULL;
  assert(mizu_pool_stop(ctrl, 30) == MIZU_TIMEOUT);
  mizu_pool_destroy(ctrl);
  ctrl = NULL;
  puts("ok stop_reaps_dead_worker");
}

/* collect_any: the first terminal handle wins, ties break to the
   earliest position; MIZU_TIMEOUT consumes nothing; a cancelled task
   reports (its index is set even on the error return). */
static void test_collect_any(void) {
  pool_pair(1, 1, 8, 64, 64, 64, 512);
  mizu_task t1 = submit_bytes(ctrl, "a", 1);
  mizu_task t2 = submit_bytes(ctrl, "b", 1);
  mizu_task t3 = submit_bytes(ctrl, "c", 1);
  mizu_task ts[3] = { t1, t2, t3 };

  size_t idx = 99;
  void *obj = NULL;
  assert(mizu_pool_collect_any(ctrl, ts, 3, &idx, &obj, 0) == MIZU_TIMEOUT);
  assert(obj == NULL);
  assert(mizu_pool_task_state(ctrl, &t1) == MIZU_RS_PENDING);   /* unconsumed */

  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);   /* two terminal: a tie */
  assert(mizu_pool_collect_any(ctrl, ts, 3, &idx, &obj, 1000) == MIZU_OK);
  assert(idx == 0);
  mizu_bytes *b = obj;
  assert(b->len == 1 && memcmp(b->data, "a", 1) == 0);
  mizu_bytes_free(b);

  assert(mizu_pool_cancel(ctrl, &t3) == 1);
  mizu_task ts2[2] = { t2, t3 };
  obj = NULL;
  assert(mizu_pool_collect_any(ctrl, ts2, 2, &idx, &obj, 1000) == MIZU_OK);
  assert(idx == 0);   /* both terminal: the earliest position */
  mizu_bytes_free(obj);
  obj = NULL;
  assert(mizu_pool_collect_any(ctrl, &t3, 1, &idx, &obj, 1000) == MIZU_ERR);
  assert(idx == 0);
  assert(strcmp(mizu_pool_error(ctrl), "task cancelled or pool stopped") == 0);
  pool_end();
  puts("ok collect_any");
}

/* collect_all: fills in input order when all OK; on the first non-OK by
   position it reports and consumes that handle alone — the OK results
   ahead of it and the handles past it stay collectible; MIZU_TIMEOUT
   consumes nothing; *err_index_out == n means all OK. */
static void test_collect_all(void) {
  pool_pair(1, 1, 8, 64, 64, 64, 512);
  mizu_task t1 = submit_bytes(ctrl, "a", 1);
  mizu_task t2 = submit_bytes(ctrl, "err", 3);
  mizu_task t3 = submit_bytes(ctrl, "c", 1);
  mizu_task ts[3] = { t1, t2, t3 };
  void *vals[3] = { (void *) 1, (void *) 1, (void *) 1 };
  size_t err = 99;
  assert(mizu_pool_collect_all(ctrl, ts, 3, vals, &err, 0) == MIZU_TIMEOUT);
  assert(vals[0] == NULL && vals[1] == NULL && vals[2] == NULL);

  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);
  err_read = 1;   /* read the ERR envelope's bytes as the value */
  assert(mizu_pool_collect_all(ctrl, ts, 3, vals, &err, 1000) == MIZU_OK);
  err_read = 0;
  assert(err == 1);
  /* only the reported handle is consumed and filled */
  assert(vals[0] == NULL && vals[2] == NULL);
  mizu_bytes *b = vals[1];
  assert(b->len == 5 && memcmp(b->data, "boom", 4) == 0);
  mizu_bytes_free(b);
  collect_bytes(ctrl, &t1, "a", 1);   /* ahead of the error: still collectible */
  collect_bytes(ctrl, &t3, "c", 1);   /* past the error: still collectible */

  mizu_task t4 = submit_bytes(ctrl, "d", 1);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);
  err = 0;
  assert(mizu_pool_collect_all(ctrl, &t4, 1, vals, &err, 1000) == MIZU_OK);
  assert(err == 1);   /* == n: all OK */
  mizu_bytes_free(vals[0]);

  /* sink form: values delivered in input order as claimed */
  mizu_task t5 = submit_bytes(ctrl, "e", 1);
  mizu_task t6 = submit_bytes(ctrl, "f", 1);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);
  mizu_task ts3[2] = { t5, t6 };
  vals[0] = vals[1] = NULL;
  err = 0;
  assert(mizu_pool_collect_all_fn(ctrl, ts3, 2, array_sink, vals, &err,
                                 1000) == MIZU_OK);
  assert(err == 2);
  mizu_bytes *b5 = vals[0], *b6 = vals[1];
  assert(b5->len == 1 && memcmp(b5->data, "e", 1) == 0);
  assert(b6->len == 1 && memcmp(b6->data, "f", 1) == 0);
  mizu_bytes_free(b5);
  mizu_bytes_free(b6);
  pool_end();
  puts("ok collect_all");
}

/* status/dump/tasks snapshots: wire-state reads plus the handle-local
   counters. */
static void test_introspection(void) {
  pool_pair(1, 1, 8, 64, 64, 64, 512);
  mizu_task t1 = submit_bytes(ctrl, "a", 1);
  mizu_task t2 = submit_bytes(ctrl, "b", 1);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);   /* t1 OK, uncollected */

  mizu_pool_status st;
  assert(mizu_pool_status_get(ctrl, &st) == MIZU_OK);
  assert(st.size == sizeof(st));
  assert(st.role == MIZU_ROLE_CONTROLLER);
  assert(st.max_workers == 1 && st.max_submitters == 8);
  assert(st.n_workers == 1 && st.n_submitters == 1);
  assert(st.worker_state[0] == MIZU_WK_LIVE);
  assert(st.sub_state[0] == MIZU_SUB_LIVE);
  assert(st.sub_state[1] == MIZU_SUB_FREE);
  assert(st.tasks_by_state[MIZU_RS_OK] == 1);
  assert(st.tasks_by_state[MIZU_RS_PENDING] == 1);
  assert(st.inj_queued[0] == 1);   /* t2 unclaimed */
  assert(st.shutdown == 0);

  mizu_pool_dump d;
  assert(mizu_pool_dump_get(ctrl, &d) == MIZU_OK);
  assert(d.size == sizeof(d));
  assert(d.n_workers == 1 && d.n_submitters == 8);
  assert(d.workers[0].pid == (int64_t) mizu_self_pid());
  assert(d.workers[0].status == MIZU_WK_LIVE);
  assert(d.submitters[0].injected == 2);
  assert(d.submitters[0].claimed == 1);
  assert(d.submitters[0].rs_count == 8);
  assert(d.help_wanted == 1);   /* no running worker to wake */
  assert(d.status.tasks_by_state[MIZU_RS_OK] == 1);

  mizu_rs_row rows[8];
  uint32_t nrows = 0;
  assert(mizu_pool_tasks_get(ctrl, rows, 8, &nrows) == MIZU_OK);
  assert(nrows == 2);
  assert(rows[0].slot == mizu_task_rs_index(&t1) && rows[0].status == MIZU_RS_OK);
  assert(rows[0].sequence == mizu_task_seq(&t1));
  assert(rows[1].slot == mizu_task_rs_index(&t2) && rows[1].status == MIZU_RS_PENDING);
  uint32_t nrows2 = 0;
  assert(mizu_pool_tasks_get(ctrl, rows, 1, &nrows2) == MIZU_OK);
  assert(nrows2 == 2);   /* the total, not the fill count */

  collect_bytes(ctrl, &t1, "a", 1);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);
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
  assert(mizu_pool_map_caps(ctrl, &free_rs, &inj_cap, &inline_entry) == 0);
  assert(free_rs == 8 && inj_cap == 64);
  assert(inline_entry == 512 - (uint32_t) sizeof(mizu_entry_hdr));

  mizu_pool_sig *sig = mizu_pool_signals(ctrl);
  assert(sig != NULL);
  assert(*sig->shutdown == 0 && *sig->owner_dead == 0);

  /* a RUNNER submit rings the doorbell (no parked worker to wake) */
  mizu_bytes b = { (void *) "r", 1 };
  mizu_task tr;
  assert(mizu_pool_submit_flags(ctrl, &b, MIZU_ENTRY_RUNNER, &tr, 0) == MIZU_OK);
  assert(*sig->help_wanted == 1);

  /* the help beat re-homes the runner onto the worker's own deque
     instead of executing it */
  exec_calls = 0;
  assert(mizu_pool_help_once(wk) == 1);
  assert(exec_calls == 0);
  assert(*sig->help_wanted == 0);   /* ring drained: the bell stays clear */
  mizu_pool_status st;
  assert(mizu_pool_status_get(ctrl, &st) == MIZU_OK);
  assert(st.deque_depth[0] == 1);
  assert(mizu_pool_help_once(wk) == 0);   /* nothing left to claim */
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);   /* pops its own deque */
  collect_bytes(ctrl, &tr, "r", 1);

  /* an ordinary claim executes inline under the help machinery */
  exec_calls = 0;
  mizu_task t0 = submit_bytes(ctrl, "z", 1);
  assert(mizu_pool_help_once(wk) == 1);
  assert(exec_calls == 1);
  collect_bytes(ctrl, &t0, "z", 1);

  /* the deque pull queues ring entries without executing them */
  mizu_task t1 = submit_bytes(ctrl, "x", 1);
  mizu_task t2 = submit_bytes(ctrl, "y", 1);
  assert(mizu_pool_deque_pull(wk, 2) == 2);
  assert(mizu_pool_status_get(ctrl, &st) == MIZU_OK);
  assert(st.deque_depth[0] == 2 && st.inj_queued[0] == 0);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);
  collect_bytes(ctrl, &t1, "x", 1);
  collect_bytes(ctrl, &t2, "y", 1);

  /* a worker's first map_caps claims its nested-submitter slot */
  assert(mizu_pool_map_caps(wk, &free_rs, NULL, NULL) == 0);
  assert(free_rs == 8);
  free(sig);
  pool_end();
  puts("ok map_support");
}

int main(void) {
  mizu_binding_bytes(&bytesb);

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
