/* Unit tier: the pool transport, exercised in-process as a controller
   plus worker/submitter handles in one process (the pool_pair/pool_step
   discipline), staging through the built-in bytes binding with a test
   exec_fn standing in for a language evaluator. Worker death is
   simulated by destroying a worker handle without leave (its liveness
   lock releases; the slot still reads LIVE — exactly a crash), so the
   reaper runs synchronously off the collect/stop probe paths.
   Assert-based; run via `make test`. */

/* _GNU_SOURCE: glibc hides nanosleep and other POSIX declarations under
   strict -std=c11; define before any system header. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE
#endif

#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <pthread.h>
#include <time.h>
#endif

#include "internal.h"

// The test binding: bytes stage/read plus a modal exec -------------------------------

static mizu_binding bytesb;
static int exec_mode = 0;  /* 0 echo, 1 strand (no publish), 2 infra fail,
                              3 abandon (eval_mark + longjmp), 4 ERR inline,
                              5 ERR tiered, 6 cancel self mid-exec */
static int err_read = 0;   /* read_fn returns the ERR envelope's bytes */
static int fail_read = 0;  /* read_fn fails the "rf" payload (no CONSUME) */
static int stage_fail = 0; /* the modal stage wrapper fails every stage */

static void array_sink(void *ctx, size_t i, void *obj) {
  ((void **) ctx)[i] = obj;
}
static int exec_calls = 0;
static jmp_buf abandon_jmp;
static mizu_pool *ctrl, *wk, *wk2, *sub;   /* defined with the harness */
static mizu_task cur_task;                 /* exec mode 6's cancel target */

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
  case 6: {
    /* cancelled between the claim and the publish: the publish CAS
       loses to CANCEL and the slot frees at pop */
    assert(mizu_pool_cancel(ctrl, &cur_task) == 1);
    const uint8_t *bytes = NULL;
    size_t n = 0;
    task_bytes(ctx, hdr, payload, limit, &bytes, &n);
    mizu_bytes b = { (void *) bytes, n };
    assert(mizu_result_publish(sink, &b) == 0);
    return 0;
  }
  case 7: {
    /* the publish's stage failure is an infrastructure failure */
    const uint8_t *bytes = NULL;
    size_t n = 0;
    task_bytes(ctx, hdr, payload, limit, &bytes, &n);
    mizu_bytes b = { (void *) bytes, n };
    assert(mizu_result_publish(sink, &b) == -1);
    return 1;
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
  if (fail_read && hdr->kind == MIZU_KIND_INLINE && hdr->len == 2 &&
      memcmp(payload, "rf", 2) == 0)
    return NULL;   /* a retryable read failure (no MIZU_READ_CONSUME) */
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

/* A stage that fails on demand: the submit-side and publish-side
   staging errors both record MIZU_ERRCAT_STAGE. */
static int maybe_fail_stage(void *obj, mizu_slot_hdr *hdr, uint8_t *payload,
                            uint32_t inline_max, mizu_handle *h, void *ctx) {
  if (stage_fail) return 1;
  return bytesb.stage(obj, hdr, payload, inline_max, h, ctx);
}

// Harness ------------------------------------------------------------------------------

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
  assert(mizu_handle_keep_out((const mizu_handle *) ctrl) == -1);
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

/* An orderly leave after an exec_fn infrastructure failure fails the
   announced in-flight claim as DIED itself — no death, no reaper: the
   collector gets the worker-death verdict and the slot frees. */
static void test_leave_fails_in_flight(void) {
  pool_pair(1, 1, 8, 64, 64, 64, 512);
  exec_mode = 2;
  mizu_task t = submit_bytes(ctrl, "x", 1);
  assert(mizu_pool_worker_run(wk) == MIZU_EXIT_ERROR);
  assert(mizu_pool_leave(wk) == MIZU_OK);

  void *obj = NULL;
  assert(mizu_pool_collect(ctrl, &t, &obj, 1000) == MIZU_ERR);
  assert(strcmp(mizu_pool_error(ctrl),
                "worker died while executing this task") == 0);
  mizu_pool_status st;
  assert(mizu_pool_status_get(ctrl, &st) == MIZU_OK);
  uint32_t live = 0;
  for (int i = 1; i < 6; i++) live += st.tasks_by_state[i];
  assert(live == 0);
  exec_mode = 0;
  pool_end();
  puts("ok leave_fails_in_flight");
}

/* leave fails every slot the worker was executing, not only the
   announced claim: the second claim overwrote the announce, so the
   first task's slot is found only by its worker_slot stamp — an orderly
   leave runs no reaper to deliver that verdict. */
static void test_leave_fails_executing(void) {
  pool_pair(1, 1, 8, 64, 64, 64, 512);
  mizu_task t1 = submit_bytes(ctrl, "x", 1);
  mizu_task t2 = submit_bytes(ctrl, "y", 1);
  exec_mode = 1;   /* claim, never publish: strands both */
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);   /* announces t1 */
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);   /* announce overwritten */
  exec_mode = 0;
  assert(mizu_pool_leave(wk) == MIZU_OK);

  void *obj = NULL;
  assert(mizu_pool_collect(ctrl, &t1, &obj, 1000) == MIZU_ERR);
  assert(strcmp(mizu_pool_error(ctrl),
                "worker died while executing this task") == 0);
  obj = NULL;
  assert(mizu_pool_collect(ctrl, &t2, &obj, 1000) == MIZU_ERR);
  assert(strcmp(mizu_pool_error(ctrl),
                "worker died while executing this task") == 0);
  pool_end();
  puts("ok leave_fails_executing");
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

/* Submitter-slot lifecycle: an orderly destroy frees the slot outright
   (nothing PENDING — no reaper needed); a destroy with PENDING work
   leaves it LIVE and the next attach's claim probe reaps it. Either way
   a live process is never denied attach by a dead registration. */
static void test_sub_slot_recycle(void) {
  pool_pair(1, 1, 2, 64, 64, 64, 512);   /* ctrl + one more submitter */
  mizu_binding b;
  make_binding(&b, 0);

  /* orderly: two attaches through one slot, each destroyed clean */
  for (int i = 0; i < 2; i++) {
    mizu_pool *s;
    assert(mizu_pool_attach(&s, token, &b) == MIZU_OK);
    char c = (char) ('a' + i);
    mizu_task t = submit_bytes(s, &c, 1);
    assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);
    collect_bytes(s, &t, &c, 1);
    mizu_pool_destroy(s);   /* nothing outstanding: slot -> FREE */
  }

  /* a PENDING task outstanding keeps the slot LIVE past the destroy;
     the claim probe reaps it (the task cancels and skips at pop) */
  mizu_pool *s;
  assert(mizu_pool_attach(&s, token, &b) == MIZU_OK);
  mizu_task doomed = submit_bytes(s, "doomed", 6);
  (void) doomed;
  mizu_pool_destroy(s);

  mizu_pool *s2;
  assert(mizu_pool_attach(&s2, token, &b) == MIZU_OK);   /* probe + reap */
  mizu_task t = submit_bytes(s2, "alive", 5);
  exec_calls = 0;
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);   /* skips the cancelled */
  assert(exec_calls == 0);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);   /* runs "alive" */
  assert(exec_calls == 1);
  collect_bytes(s2, &t, "alive", 5);
  mizu_pool_destroy(s2);
  pool_end();
  puts("ok sub_slot_recycle");
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
  size_t err = 99, consumed = 99;
  assert(mizu_pool_collect_all(ctrl, ts, 3, vals, &err, &consumed, 0) ==
         MIZU_TIMEOUT);
  assert(vals[0] == NULL && vals[1] == NULL && vals[2] == NULL);
  assert(consumed == 0);

  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);
  err_read = 1;   /* read the ERR envelope's bytes as the value */
  assert(mizu_pool_collect_all(ctrl, ts, 3, vals, &err, &consumed, 1000) ==
         MIZU_OK);
  err_read = 0;
  assert(err == 1 && consumed == 0);
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
  assert(mizu_pool_collect_all(ctrl, &t4, 1, vals, &err, &consumed, 1000) ==
         MIZU_OK);
  assert(err == 1 && consumed == 1);   /* err == n: all OK */
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
                                 &consumed, 1000) == MIZU_OK);
  assert(err == 2 && consumed == 2);
  mizu_bytes *b5 = vals[0], *b6 = vals[1];
  assert(b5->len == 1 && memcmp(b5->data, "e", 1) == 0);
  assert(b6->len == 1 && memcmp(b6->data, "f", 1) == 0);
  mizu_bytes_free(b5);
  mizu_bytes_free(b6);

  /* a read failure mid-claim: MIZU_ERR names the failing slot (unconsumed
     when retryable) and carries the consumed prefix out */
  mizu_task u1 = submit_bytes(ctrl, "ok", 2);
  mizu_task u2 = submit_bytes(ctrl, "rf", 2);
  mizu_task u3 = submit_bytes(ctrl, "ok", 2);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);
  mizu_task us[3] = { u1, u2, u3 };
  void *uvals[3] = { NULL, NULL, NULL };
  err = 99;
  consumed = 99;
  fail_read = 1;
  assert(mizu_pool_collect_all(ctrl, us, 3, uvals, &err, &consumed, 1000) ==
         MIZU_ERR);
  fail_read = 0;
  assert(err == 1 && consumed == 1);
  mizu_bytes *ub = uvals[0];
  assert(ub->len == 2 && memcmp(ub->data, "ok", 2) == 0);
  mizu_bytes_free(ub);
  assert(uvals[1] == NULL && uvals[2] == NULL);
  collect_bytes(ctrl, &u2, "rf", 2);  /* the failing slot: retryable */
  collect_bytes(ctrl, &u3, "ok", 2);  /* past the failure: collectible */
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

// The zc binding: bytes plus a spill-region zc loan past the inline budget ---

static mizu_shm *zc_last;      /* the region the last zc stage retained */
static int zc_mode = 0;        /* 0 echo, 1 strand (no publish), 2 lost */

static int zc_stage(void *obj, mizu_slot_hdr *hdr, uint8_t *payload,
                    uint32_t inline_max, mizu_handle *h, void *ctx) {
  (void) ctx;
  mizu_bytes *b = obj;
  if (b->len <= inline_max) {
    hdr->kind = MIZU_KIND_INLINE;
    hdr->len = (uint32_t) b->len;
    hdr->aux = 0;
    memcpy(payload, b->data, b->len);
    return 0;
  }
  mizu_shm *shm;
  if (mizu_stage_spill_get(h, 64 + b->len, &shm) != MIZU_OK) return 1;
  memcpy((unsigned char *) shm->addr + 64, b->data, b->len);
  mizu_stage_retain_zc(h, shm);          /* an SHM_VEC-style zc loan */
  hdr->kind = MIZU_KIND_SHM_RAW;
  hdr->len = shm->name_len;
  hdr->aux = b->len;
  memcpy(payload, shm->name, shm->name_len);
  zc_last = shm;
  return 0;
}

static void *zc_read(const mizu_slot_hdr *hdr, const uint8_t *payload,
                     size_t limit, mizu_read_ctx *ctx) {
  (void) limit;
  if (ctx->outcome != MIZU_RS_OK) return NULL;
  mizu_bytes *b = malloc(sizeof *b);
  if (hdr->kind == MIZU_KIND_INLINE) {
    b->data = malloc(hdr->len);
    b->len = hdr->len;
    memcpy(b->data, payload, hdr->len);
    return b;
  }
  mizu_shm *shm = mizu_read_region(ctx, payload, hdr->len);
  assert(shm != NULL);
  b->data = malloc(hdr->aux);
  b->len = hdr->aux;
  memcpy(b->data, (unsigned char *) shm->addr + 64, hdr->aux);
  return b;
}

static int zc_exec(const mizu_slot_hdr *hdr, const uint8_t *payload,
                   size_t limit, mizu_result_sink *sink, int catching,
                   mizu_read_ctx *ctx) {
  (void) limit;
  (void) catching;
  if (zc_mode == 1) return 0;              /* claim, never publish */
  if (zc_mode == 2) {                      /* the payload-vanished terminal */
    mizu_result_publish_lost(sink);
    return 0;
  }
  if (hdr->kind == MIZU_KIND_SHM_RAW) {    /* a big zc task arg: ack it */
    mizu_shm *shm = mizu_read_region(ctx, payload, hdr->len);
    assert(shm != NULL);
    mizu_bytes b = { (void *) "ack", 3 };
    assert(mizu_result_publish(sink, &b) == 1);
    return 0;
  }
  assert(hdr->kind == MIZU_KIND_INLINE);
  if (hdr->len == 3 && memcmp(payload, "big", 3) == 0) {
    static unsigned char big[1000];
    memset(big, 0xAB, sizeof big);
    mizu_bytes b = { big, sizeof big };
    assert(mizu_result_publish(sink, &b) == 1);
    return 0;
  }
  mizu_bytes b = { (void *) payload, hdr->len };
  assert(mizu_result_publish(sink, &b) == 1);
  return 0;
}

static void make_zc_binding(mizu_binding *b, int worker) {
  mizu_binding_init(b);
  b->stage = zc_stage;
  b->read = zc_read;
  b->ident = MIZU_IDENT(MIZU_LANG_BYTES, 0);
  if (worker) b->exec = zc_exec;
}

static void zc_pool_pair(uint32_t workers, uint32_t joined, uint32_t max_sub,
                         uint32_t inj, uint32_t dq, uint32_t rs,
                         uint32_t slot) {
  mizu_pool_opts opts;
  mizu_pool_opts_init(&opts);
  opts.max_workers = workers;
  opts.max_submitters = max_sub;
  opts.injection_cap = inj;
  opts.per_worker_cap = dq;
  opts.result_slots = rs;
  opts.slot_size = slot;
  mizu_binding b, wbv;
  make_zc_binding(&b, 0);
  make_zc_binding(&wbv, 1);
  assert(mizu_pool_create(&ctrl, &opts, &b) == MIZU_OK);
  assert(mizu_pool_token(ctrl, token, sizeof(token)) == MIZU_OK);
  assert(mizu_pool_worker_join(&wk, token, 0, &wbv) == MIZU_OK);
  wk2 = NULL;
  if (joined > 1)
    assert(mizu_pool_worker_join(&wk2, token, 1, &wbv) == MIZU_OK);
  sub = NULL;
}

static void spill_info(mizu_pool *p, uint32_t *fl, uint32_t *led) {
  mizu_handle_spill_info((const mizu_handle *) p, fl, led);
}

/* The lent-region ledger keys a worker handle's result regions by
   consumer kind (submitter slot) and its task-arg regions by kind
   (worker slot): a worker-death force-reclaim of slot k must not
   reclaim a result region lent to submitter k. */
static void test_ledger_key_kinds(void) {
  zc_pool_pair(2, 2, 3, 64, 64, 6, 256);
  mizu_binding sb;
  make_zc_binding(&sb, 0);
  assert(mizu_pool_attach(&sub, token, &sb) == MIZU_OK);   /* submitter 1 */

  /* submitter 1 gets a zc result from worker 0 and still "holds a view" */
  mizu_bytes req = { (void *) "big", 3 };
  mizu_task t;
  assert(mizu_pool_submit(sub, &req, &t, 1000) == MIZU_OK);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);
  mizu_shm *region = zc_last;
  mizu_zc_ref(region);                    /* S's view wrap: rc 1 -> 2 */
  void *obj = NULL;
  assert(mizu_pool_collect(sub, &t, &obj, 1000) == MIZU_OK);
  free(((mizu_bytes *) obj)->data);
  free(obj);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_IDLE);   /* the keeper reap */
  uint32_t fl, led;
  spill_info(wk, &fl, &led);
  assert(fl == 0 && led == 1);     /* lent to submitter 1: keyed (SUB, 1) */

  /* worker slot 1 claims a nested task of wk and dies */
  zc_mode = 1;
  mizu_bytes n = { (void *) "n", 1 };
  mizu_task tn;
  assert(mizu_pool_submit(wk, &n, &tn, 0) == MIZU_OK);
  assert(mizu_pool_step(wk2, 0) == MIZU_STEP_TASK);   /* stolen, stranded */
  zc_mode = 0;
  mizu_pool_destroy(wk2);                             /* crash */
  wk2 = NULL;

  /* wk's nested collect reaps worker 1 (backstop probe) and claims DIED */
  obj = NULL;
  assert(mizu_pool_collect(wk, &tn, &obj, 1500) == MIZU_ERR);
  assert(strcmp(mizu_pool_error(wk),
                "worker died while executing this task") == 0);
  spill_info(wk, &fl, &led);
  assert(led == 1 && fl == 0);   /* the (SUB, 1) entry: a different
                                    namespace, untouched by the force */

  /* the held region never recycled: a later big stage gets a fresh one */
  mizu_task t3;
  assert(mizu_pool_submit(sub, &req, &t3, 1000) == MIZU_OK);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);
  assert(zc_last != region);
  obj = NULL;
  assert(mizu_pool_collect(sub, &t3, &obj, 1000) == MIZU_OK);
  free(((mizu_bytes *) obj)->data);
  free(obj);

  mizu_zc_unref(region);   /* the simulated view releases; t3's region
                              keeps only its producer loan (released at
                              wk's leave) */
  pool_end();
  puts("ok ledger_key_kinds");
}

/* A payload-vanished publish is LOST, not DIED: the claim frees the slot
   with the lost message and no claimant record, and performs no
   force-reclaim of the (live) claimant's lent regions. */
static void test_lost_task(void) {
  zc_pool_pair(1, 1, 1, 64, 64, 8, 512);

  /* a big zc task arg the worker "still views" sits in ctrl's ledger,
     keyed to its consumer worker 0 */
  static unsigned char big[1000];
  memset(big, 0x5A, sizeof big);
  mizu_bytes arg = { big, sizeof big };
  mizu_task t1;
  assert(mizu_pool_submit(ctrl, &arg, &t1, 1000) == MIZU_OK);
  mizu_shm *region = zc_last;
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);
  mizu_zc_ref(region);                    /* the worker's live view wrap */
  void *obj = NULL;
  assert(mizu_pool_collect(ctrl, &t1, &obj, 1000) == MIZU_OK);
  free(((mizu_bytes *) obj)->data);
  free(obj);
  uint32_t fl, led;
  spill_info(ctrl, &fl, &led);
  assert(led == 1);

  /* the worker's payload-vanished publish claims LOST: no force */
  zc_mode = 2;
  mizu_task t2 = submit_bytes(ctrl, "x", 1);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);
  obj = NULL;
  assert(mizu_pool_collect(ctrl, &t2, &obj, 500) == MIZU_ERR);
  assert(strcmp(mizu_pool_error(ctrl),
                "task lost: payload region vanished") == 0);
  assert(mizu_pool_task_state(ctrl, &t2) == MIZU_RS_FREE);
  spill_info(ctrl, &fl, &led);
  assert(led == 1);   /* the live claimant's entry stays GC-gated */
  zc_mode = 0;

  mizu_zc_unref(region);
  pool_end();
  puts("ok lost_task");
}

#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>

/* A worker dying between the announce and the claim CAS leaves its entry
   queued; the reaper fails the slot DIED and the submitter reuses it.
   The zombie guard (the entry's incarnation stamp): the still-queued
   entry never executes against the slot's new incarnation. */
static void test_zombie_entry(void) {
  mizu_pool_opts opts;
  mizu_pool_opts_init(&opts);
  opts.max_workers = 1;
  opts.max_submitters = 1;
  opts.injection_cap = 64;
  opts.per_worker_cap = 64;
  opts.result_slots = 1;
  opts.slot_size = 512;
  mizu_binding b, wb;
  make_binding(&b, 0);
  make_binding(&wb, 1);
  assert(mizu_pool_create(&ctrl, &opts, &b) == MIZU_OK);
  assert(mizu_pool_token(ctrl, token, sizeof(token)) == MIZU_OK);

  /* the worker child: joins slot 0, then "dies" on signal — the slot is
     left LIVE with the lock released, exactly a crash */
  int go[2];
  assert(pipe(go) == 0);
  pid_t pid = fork();
  assert(pid >= 0);
  if (pid == 0) {
    close(go[1]);
    mizu_pool *cwk;
    if (mizu_pool_worker_join(&cwk, token, 0, &wb) != MIZU_OK) _exit(2);
    char c;
    if (read(go[0], &c, 1) != 1) _exit(3);
    _exit(0);
  }
  close(go[0]);
  uint32_t s0 = 0;
  assert(mizu_pool_ready_wait(ctrl, &s0, 1, 5000) == MIZU_OK);  /* arms the
                                                                   watch */

  mizu_task t1 = submit_bytes(ctrl, "first", 5);   /* queued, never claimed */

  /* the announce (pool_announce) without the claim CAS, written through a
     fresh mapping: the fields are plain shared memory */
  char name[64];
  int nl = snprintf(name, sizeof(name), "%s%s", MIZU_PREFIX_LITERAL, token);
  assert(nl > 0 && (size_t) nl < sizeof(name));
  mizu_shm *shm = mizu_shm_open_rw_heap(name, 0);
  assert(shm != NULL);
  unsigned char *base = shm->addr;
  mizu_pool_hdr h;
  memcpy(&h, base, sizeof(h));
  mizu_wk_slot *w0 = (mizu_wk_slot *) (base + 64);
  size_t results_off = 64 + (size_t) h.max_workers * sizeof(mizu_wk_slot) +
    (size_t) h.max_submitters * sizeof(mizu_sub_slot) + 128 +
    (size_t) h.max_submitters *
      (MIZU_INJ_META_SIZE + (size_t) h.inj_cap * h.slot) +
    (size_t) h.max_workers * ((size_t) h.deque_cap * h.slot);
  mizu_rs_hdr *rs0 = (mizu_rs_hdr *) (base + results_off);
  atomic_store(&w0->in_flight_seq, atomic_load(&rs0->sequence));
  atomic_store(&w0->in_flight_rs, 0);

  assert(write(go[1], "g", 1) == 1);
  close(go[1]);
  int wst;
  assert(waitpid(pid, &wst, 0) == pid);

  /* the death-callback reap fails the announced incarnation */
  int st = 0, died = 0;
  MIZU_SPIN_WAIT((st = mizu_pool_task_state(ctrl, &t1)) == MIZU_RS_DIED,
                 mizu_now() + 5, MIZU_SPIN_CLOCK_EVERY_LIGHT, died);
  assert(died && st == MIZU_RS_DIED);
  void *obj = NULL;
  assert(mizu_pool_collect(ctrl, &t1, &obj, 1000) == MIZU_ERR);
  assert(strcmp(mizu_pool_error(ctrl),
                "worker died while executing this task") == 0);
  mizu_pool_status ps;
  int freed = 0;
  MIZU_SPIN_WAIT((mizu_pool_status_get(ctrl, &ps) == MIZU_OK &&
                  ps.worker_state[0] == MIZU_WK_FREE),
                 mizu_now() + 5, MIZU_SPIN_CLOCK_EVERY_LIGHT, freed);
  assert(freed);

  /* slot reuse: the zombie entry (incarnation 1) queues ahead of the new
     one (incarnation 2) — the guard drops it without executing */
  mizu_task t2 = submit_bytes(ctrl, "second", 6);
  assert(mizu_pool_worker_join(&wk, token, 0, &wb) == MIZU_OK);
  exec_calls = 0;
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);   /* the zombie */
  assert(exec_calls == 0);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);   /* the real entry */
  assert(exec_calls == 1);
  collect_bytes(ctrl, &t2, "second", 6);

  mizu_shm_close_stack(shm, 0);
  free(shm);
  pool_end();
  puts("ok zombie_entry");
}
#endif

/* Create validation: the binding check, then every opts gate and the
   liveness-dir budget, refuses through the TLS slot. */
static void test_create_validate(void) {
  mizu_pool *bad = NULL;
  mizu_pool_opts o;
  mizu_pool_opts_init(&o);
  o.max_workers = 1;
  o.max_submitters = 8;
  o.injection_cap = 64;
  o.per_worker_cap = 64;
  o.result_slots = 64;
  o.slot_size = 512;
  mizu_binding b;
  make_binding(&b, 0);

  mizu_binding nb;
  mizu_binding_init(&nb);   /* no stage/read */
  assert(mizu_pool_create(&bad, &o, &nb) == MIZU_ERR);
  nb = b;
  nb.ident = 0;             /* a zero language byte */
  assert(mizu_pool_create(&bad, &o, &nb) == MIZU_ERR);

  o.max_workers = 0;
  assert(mizu_pool_create(&bad, &o, &b) == MIZU_ERR);
  o.max_workers = MIZU_MAX_WORKERS + 1;
  assert(mizu_pool_create(&bad, &o, &b) == MIZU_ERR);
  o.max_workers = 1;
  o.max_submitters = 0;
  assert(mizu_pool_create(&bad, &o, &b) == MIZU_ERR);
  o.max_submitters = 65;
  assert(mizu_pool_create(&bad, &o, &b) == MIZU_ERR);
  o.max_submitters = 8;
  o.injection_cap = 3;
  assert(mizu_pool_create(&bad, &o, &b) == MIZU_ERR);
  o.injection_cap = 64;
  o.per_worker_cap = 3;
  assert(mizu_pool_create(&bad, &o, &b) == MIZU_ERR);
  o.per_worker_cap = 64;
  o.slot_size = 64;         /* below the 128 floor */
  assert(mizu_pool_create(&bad, &o, &b) == MIZU_ERR);
  o.slot_size = 1000;       /* not a power of two */
  assert(mizu_pool_create(&bad, &o, &b) == MIZU_ERR);
  o.slot_size = 2u << 20;   /* past the 2^20 cap */
  assert(mizu_pool_create(&bad, &o, &b) == MIZU_ERR);
  o.slot_size = 512;
  o.result_slots = 4;       /* below max_submitters */
  assert(mizu_pool_create(&bad, &o, &b) == MIZU_ERR);
  o.result_slots = 64;

#ifndef _WIN32
  /* a liveness directory path past the wire budget refuses */
  char longdir[1000];
  memset(longdir, 'd', sizeof(longdir) - 1);
  longdir[sizeof(longdir) - 1] = '\0';
  assert(setenv("MIZU_LIVENESS_DIR", longdir, 1) == 0);
  assert(mizu_pool_create(&bad, &o, &b) == MIZU_ERR);
  assert(strstr(mizu_last_error_message(),
                "liveness directory path too long") != NULL);
  unsetenv("MIZU_LIVENESS_DIR");
#endif
  assert(bad == NULL);

  /* the token: NULL and released handles, a short buffer */
  char tok[64];
  assert(mizu_pool_token(NULL, tok, sizeof(tok)) == MIZU_ERR);
  pool_pair(1, 1, 8, 64, 64, 64, 512);
  char tiny[2];
  assert(mizu_pool_token(ctrl, tiny, sizeof(tiny)) == MIZU_ERR);
  assert(mizu_pool_leave(wk) == MIZU_OK);
  assert(mizu_pool_stop(ctrl, 1000) == MIZU_OK);
  assert(mizu_pool_token(ctrl, tok, sizeof(tok)) == MIZU_ERR);
  mizu_pool_destroy(wk);
  wk = NULL;
  mizu_pool_destroy(ctrl);
  ctrl = NULL;
  puts("ok create_validate");
}

/* The wire header validator: one crafted header, every reject branch. */
static void test_hdr_validate(void) {
  pool_pair(1, 1, 8, 64, 64, 64, 512);
  /* read the live header back through the region */
  char name[80];
  int nl = snprintf(name, sizeof(name), MIZU_PREFIX_LITERAL "%s", token);
  assert(nl > 0 && (size_t) nl < sizeof(name));
  mizu_shm *shm = NULL;
  assert(mizu_shm_open_rw(&shm, name, 0) == MIZU_OK);
  mizu_pool_hdr h;
  memcpy(&h, mizu_shm_addr(shm), sizeof(h));
  const size_t region_size = mizu_shm_size(shm);

  mizu_pool_hdr v;
  assert(mizu_pool_hdr_validate(&h, region_size, &v) == NULL);
  assert(mizu_pool_hdr_validate(&h, 32, NULL) != NULL);

  mizu_pool_hdr bad = h;
  bad.magic += 1;
  assert(mizu_pool_hdr_validate(&bad, region_size, NULL) != NULL);
  bad = h;
  bad.version += 1;
  assert(mizu_pool_hdr_validate(&bad, region_size, NULL) != NULL);
  bad = h;
  bad.max_workers = 0;
  assert(mizu_pool_hdr_validate(&bad, region_size, NULL) != NULL);
  bad = h;
  bad.max_submitters = 65;
  assert(mizu_pool_hdr_validate(&bad, region_size, NULL) != NULL);
  bad = h;
  bad.inj_cap = 3;
  assert(mizu_pool_hdr_validate(&bad, region_size, NULL) != NULL);
  bad = h;
  bad.deque_cap = 0;
  assert(mizu_pool_hdr_validate(&bad, region_size, NULL) != NULL);
  bad = h;
  bad.slot = 64;
  assert(mizu_pool_hdr_validate(&bad, region_size, NULL) != NULL);
  bad = h;
  bad.result_slots = 7;   /* not a multiple of max_submitters */
  assert(mizu_pool_hdr_validate(&bad, region_size, NULL) != NULL);
  bad = h;
  assert(mizu_pool_hdr_validate(&bad, 128, NULL) != NULL);   /* too small */
  bad = h;
  bad.livedir_offset = region_size + 64;
  assert(mizu_pool_hdr_validate(&bad, region_size, NULL) != NULL);
  bad = h;
  bad.livedir_size = 0;
  assert(mizu_pool_hdr_validate(&bad, region_size, NULL) != NULL);
  bad = h;
  bad.livedir_size = 901;
  assert(mizu_pool_hdr_validate(&bad, region_size, NULL) != NULL);

  mizu_shm_close(shm, 0);
  pool_end();
  puts("ok hdr_validate");
}

/* The handle guards: NULL and released handles answer with the TLS slot
   or the handle's error record; role mismatches refuse. */
static void test_handle_guards(void) {
  pool_pair(1, 1, 8, 64, 64, 64, 512);
  mizu_bytes b = { (void *) "x", 1 };
  mizu_task t = submit_bytes(ctrl, "x", 1);
  void *obj = NULL;
  size_t idx = 9, err = 9, consumed = 9, n_out = 9;
  mizu_pool_status st;
  mizu_pool_dump d;
  mizu_rs_row rows[1];
  uint32_t nrows = 9;
  mizu_pool_sig *sig;
  uint32_t free_rs, inj_cap, inline_entry;

  /* role mismatches on live handles */
  assert(mizu_pool_leave(ctrl) == MIZU_ERR);         /* not a worker */
  assert(mizu_pool_help_once(ctrl) == -1);
  assert(mizu_pool_deque_pull(ctrl, 1) == -1);
  assert(mizu_pool_step(ctrl, 0) == MIZU_STEP_SHUTDOWN);
  assert(mizu_pool_worker_run(ctrl) == MIZU_EXIT_ERROR);
  assert(mizu_pool_stop(wk, 0) == MIZU_ERR);         /* not the controller */
  assert(mizu_pool_ready_wait(wk, NULL, 0, 0) == MIZU_ERR);
  assert(mizu_pool_retire(wk, 0) == MIZU_ERR);
  assert(mizu_pool_retire(ctrl, 9) == MIZU_ERR);     /* out of range */
  assert(mizu_pool_collect(wk, &t, &obj, 0) == MIZU_ERR);   /* not ours */

  /* invalid task handles */
  mizu_task bogus = mizu_task_make(1, 9999);
  assert(mizu_pool_collect(ctrl, &bogus, &obj, 0) == MIZU_ERR);
  assert(mizu_pool_collect(ctrl, NULL, &obj, 0) == MIZU_ERR);
  assert(mizu_pool_cancel(ctrl, &bogus) == 0);
  assert(mizu_pool_task_release(ctrl, &bogus) == 0);
  assert(mizu_pool_task_state(ctrl, &bogus) == MIZU_RS_FREE);
  assert(mizu_pool_collect_any(ctrl, &bogus, 1, &idx, &obj, 0) == MIZU_ERR);
  assert(mizu_pool_collect_any(ctrl, &t, 1, NULL, &obj, 0) == MIZU_ERR);
  assert(mizu_pool_collect_any(ctrl, NULL, 1, &idx, &obj, 0) == MIZU_ERR);
  assert(mizu_pool_collect_any(ctrl, &t, 0, &idx, &obj, 0) == MIZU_ERR);
  assert(mizu_pool_collect_all(ctrl, NULL, 1, &obj, &err, &consumed,
                               0) == MIZU_ERR);
  assert(mizu_pool_submit_flags(ctrl, &b, 0, NULL, 0) == MIZU_ERR);
  assert(mizu_pool_submit_batch_fn(ctrl, NULL, NULL, 1, &t, &n_out,
                                   0) == MIZU_ERR);
  assert(mizu_pool_status_get(NULL, &st) == MIZU_ERR);
  assert(mizu_pool_status_get(ctrl, NULL) == MIZU_ERR);
  assert(mizu_pool_dump_get(NULL, &d) == MIZU_ERR);
  assert(mizu_pool_dump_get(ctrl, NULL) == MIZU_ERR);
  assert(mizu_pool_tasks_get(NULL, rows, 1, &nrows) == MIZU_ERR);
  assert(mizu_pool_tasks_get(ctrl, rows, 1, NULL) == MIZU_ERR);
  assert(mizu_pool_tasks_get(ctrl, NULL, 1, &nrows) == MIZU_ERR);

  /* NULL handles outright */
  assert(mizu_pool_submit(NULL, &b, &t, 0) == MIZU_ERR);
  assert(mizu_pool_submit_batch_fn(NULL, NULL, NULL, 0, &t, &n_out,
                                   0) == MIZU_ERR);
  assert(mizu_pool_collect(NULL, &t, &obj, 0) == MIZU_ERR);
  assert(mizu_pool_collect_any(NULL, &t, 1, &idx, &obj, 0) == MIZU_ERR);
  assert(mizu_pool_collect_all(NULL, &t, 1, &obj, &err, &consumed,
                               0) == MIZU_ERR);
  assert(mizu_pool_cancel(NULL, &t) == 0);
  assert(mizu_pool_task_release(NULL, &t) == 0);
  assert(mizu_pool_task_state(NULL, &t) == MIZU_RS_FREE);
  assert(mizu_pool_map_caps(NULL, &free_rs, &inj_cap, &inline_entry) == -1);
  assert(mizu_pool_signals(NULL) == NULL);
  assert(mizu_pool_help_once(NULL) == -1);
  assert(mizu_pool_deque_pull(NULL, 1) == -1);
  assert(mizu_pool_leave(NULL) == MIZU_OK);
  assert(mizu_pool_stop(NULL, 0) == MIZU_OK);
  assert(mizu_pool_retire(NULL, 0) == MIZU_ERR);
  assert(mizu_pool_ready_wait(NULL, NULL, 0, 0) == MIZU_ERR);
  assert(mizu_pool_worker_ident(NULL) == 0);
  assert(mizu_pool_lame_duck(NULL) == 1);
  mizu_pool_destroy(NULL);   /* no-op */
  (void) sig;

  /* released handles: stop released the controller */
  mizu_task live = submit_bytes(ctrl, "y", 1);
  (void) live;
  assert(mizu_pool_leave(wk) == MIZU_OK);
  assert(mizu_pool_stop(ctrl, 1000) == MIZU_OK);
  assert(mizu_pool_submit(ctrl, &b, &t, 0) == MIZU_ERR);
  assert(mizu_pool_collect(ctrl, &t, &obj, 0) == MIZU_ERR);
  assert(mizu_pool_status_get(ctrl, &st) == MIZU_ERR);
  assert(mizu_pool_dump_get(ctrl, &d) == MIZU_ERR);
  assert(mizu_pool_tasks_get(ctrl, rows, 1, &nrows) == MIZU_ERR);
  assert(mizu_pool_token(ctrl, token, sizeof(token)) == MIZU_ERR);
  assert(mizu_pool_signals(ctrl) == NULL);
  assert(mizu_pool_map_caps(ctrl, &free_rs, NULL, NULL) == -1);
  assert(mizu_pool_stop(ctrl, 0) == MIZU_OK);   /* idempotent */
  mizu_pool_destroy(wk);
  wk = NULL;
  mizu_pool_destroy(ctrl);
  ctrl = NULL;
  puts("ok handle_guards");
}

/* Join failures: slot range, the held lock (a live worker), the ghost
   slot (a dead one), the language word, and the full submitter
   registry. */
static void test_join_errors(void) {
  pool_pair(2, 1, 2, 64, 64, 64, 512);
  mizu_binding wb, b;
  make_binding(&wb, 1);
  make_binding(&b, 0);
  mizu_pool *x = NULL;

  assert(mizu_pool_worker_join(&x, token, 2, &wb) == MIZU_ERR);
  assert(strstr(mizu_last_error_message(), "out of range") != NULL);
  assert(mizu_pool_worker_join(&x, token, 0, &wb) == MIZU_ERR);
  assert(strstr(mizu_last_error_message(), "already held") != NULL);
  assert(mizu_pool_retire(ctrl, 1) == MIZU_ERR);   /* slot never joined */
  assert(strstr(mizu_pool_error(ctrl), "is not live") != NULL);

  /* a dead worker's slot is LIVE: the join fail-fasts on the ghost */
  mizu_pool_destroy(wk);   /* simulated crash */
  wk = NULL;
  assert(mizu_pool_worker_join(&x, token, 0, &wb) == MIZU_ERR);
  assert(strstr(mizu_last_error_message(), "not free") != NULL);

  /* a binding with a different identity word cannot join */
  mizu_binding fb;
  make_binding(&fb, 1);
  fb.ident = MIZU_IDENT(MIZU_LANG_BYTES + 1, 0);
  assert(mizu_pool_worker_join(&x, token, 1, &fb) == MIZU_ERR);
  assert(strstr(mizu_last_error_message(), "language") != NULL);

  /* two submitters of two: the third attach probes both LIVE slots and
     reports the full registry */
  mizu_pool *s1;
  assert(mizu_pool_attach(&s1, token, &b) == MIZU_OK);
  mizu_pool *s2;
  assert(mizu_pool_attach(&s2, token, &b) == MIZU_ERR);
  assert(strstr(mizu_last_error_message(), "submitter registry full") !=
         NULL);
  mizu_pool_destroy(s1);

  /* the pool survives: a clean join on a fresh slot works */
  assert(mizu_pool_worker_join(&wk, token, 1, &wb) == MIZU_OK);
  mizu_task t = submit_bytes(ctrl, "ok", 2);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);
  collect_bytes(ctrl, &t, "ok", 2);
  pool_end();
  puts("ok join_errors");
}

/* A worker joined without an evaluator: step/run refuse, and the
   full-deque nested submit errors instead of executing inline. */
static void test_nested_no_exec(void) {
  mizu_pool_opts opts;
  mizu_pool_opts_init(&opts);
  opts.max_workers = 1;
  opts.max_submitters = 8;
  opts.injection_cap = 64;
  opts.per_worker_cap = 2;
  opts.result_slots = 64;
  opts.slot_size = 512;
  mizu_binding b, nb;
  make_binding(&b, 0);
  mizu_binding_init(&nb);   /* stage/read but no exec */
  nb.stage = bytesb.stage;
  nb.read = test_read;
  nb.ident = MIZU_IDENT(MIZU_LANG_BYTES, 0);
  assert(mizu_pool_create(&ctrl, &opts, &b) == MIZU_OK);
  assert(mizu_pool_token(ctrl, token, sizeof(token)) == MIZU_OK);
  assert(mizu_pool_worker_join(&wk, token, 0, &nb) == MIZU_OK);

  assert(mizu_pool_step(wk, 0) == MIZU_STEP_SHUTDOWN);
  assert(strstr(mizu_pool_error(wk), "no evaluator") != NULL);
  assert(mizu_pool_worker_run(wk) == MIZU_EXIT_ERROR);

  mizu_bytes x = { (void *) "n", 1 };
  mizu_task t1, t2, t3;
  assert(mizu_pool_submit(wk, &x, &t1, 0) == MIZU_OK);   /* deque: 1 */
  assert(mizu_pool_submit(wk, &x, &t2, 0) == MIZU_OK);   /* deque: 2 (full) */
  assert(mizu_pool_submit(wk, &x, &t3, 0) == MIZU_ERR);  /* inline: no exec */
  assert(strstr(mizu_pool_error(wk), "no evaluator") != NULL);

  assert(mizu_pool_leave(wk) == MIZU_OK);
  mizu_pool_destroy(wk);
  wk = NULL;
  mizu_pool_destroy(ctrl);
  ctrl = NULL;
  puts("ok nested_no_exec");
}

/* Staging failures: at submit (the slot never commits), at the nested
   submit, and at the publish (an infrastructure failure — the worker
   goes down). */
static void test_stage_failure(void) {
  mizu_binding fb, fwb;
  make_binding(&fb, 0);
  make_binding(&fwb, 1);
  fb.stage = maybe_fail_stage;
  fwb.stage = maybe_fail_stage;
  mizu_pool_opts opts;
  mizu_pool_opts_init(&opts);
  opts.max_workers = 1;
  opts.max_submitters = 8;
  opts.injection_cap = 64;
  opts.per_worker_cap = 64;
  opts.result_slots = 64;
  opts.slot_size = 512;
  assert(mizu_pool_create(&ctrl, &opts, &fb) == MIZU_OK);
  assert(mizu_pool_token(ctrl, token, sizeof(token)) == MIZU_OK);
  assert(mizu_pool_worker_join(&wk, token, 0, &fwb) == MIZU_OK);

  mizu_bytes b = { (void *) "x", 1 };
  mizu_task t;
  stage_fail = 1;
  assert(mizu_pool_submit(ctrl, &b, &t, 0) == MIZU_ERR);
  assert(mizu_pool_errcat(ctrl) == MIZU_ERRCAT_STAGE);
  stage_fail = 0;
  t = submit_bytes(ctrl, "x", 1);                        /* recovered */
  mizu_task t2 = submit_bytes(ctrl, "y", 1);

  stage_fail = 1;
  mizu_task tn;
  assert(mizu_pool_submit(wk, &b, &tn, 0) == MIZU_ERR);  /* nested */
  assert(mizu_pool_errcat(wk) == MIZU_ERRCAT_STAGE);
  stage_fail = 0;

  /* the publish's stage failure is an infrastructure failure: the
     worker goes down mid-task, the next claim carries on */
  exec_mode = 7;
  stage_fail = 1;
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_SHUTDOWN);   /* claims t */
  assert(mizu_pool_errcat(wk) == MIZU_ERRCAT_OTHER);     /* the exec record */
  assert(strstr(mizu_pool_error(wk), "exec_fn failed") != NULL);
  stage_fail = 0;
  exec_mode = 0;
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);       /* t2 */
  collect_bytes(ctrl, &t2, "y", 1);

  /* the stranded task fails DIED at the worker's leave */
  assert(mizu_pool_leave(wk) == MIZU_OK);
  void *obj = NULL;
  assert(mizu_pool_collect(ctrl, &t, &obj, 1000) == MIZU_ERR);
  assert(strcmp(mizu_pool_error(ctrl),
                "worker died while executing this task") == 0);
  mizu_pool_destroy(ctrl);
  ctrl = NULL;
  mizu_pool_destroy(wk);
  wk = NULL;
  puts("ok stage_failure");
}

/* The vectored collects: the park/timeout cycle consuming nothing, the
   dead-claimant backstop probe, and the help modes (a blocked worker's
   own pop, then a steal from a peer's deque). */
static void test_vectored_collect(void) {
  pool_pair(2, 2, 8, 64, 64, 64, 512);
  size_t idx = 9, err = 9, consumed = 9;
  void *obj = NULL;
  void *vals[2] = { NULL, NULL };

  /* the park cycle: announced, slept, deadline — nothing consumed */
  mizu_task t1 = submit_bytes(ctrl, "a", 1);
  mizu_task t2 = submit_bytes(ctrl, "b", 1);
  mizu_task ts[2] = { t1, t2 };
  assert(mizu_pool_collect_any(ctrl, ts, 2, &idx, &obj, 30) ==
         MIZU_TIMEOUT);
  assert(obj == NULL);
  assert(mizu_pool_task_state(ctrl, &t1) == MIZU_RS_PENDING);
  assert(mizu_pool_collect_all(ctrl, ts, 2, vals, &err, &consumed, 30) ==
         MIZU_TIMEOUT);
  assert(consumed == 0);

  /* a claimed-but-stranded task: the wake backstop probes the claimant,
     which is alive here — the wait times out anyway */
  exec_mode = 1;   /* claim, never publish */
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);
  exec_mode = 0;
  assert(mizu_pool_collect_any(ctrl, ts, 2, &idx, &obj, 60) ==
         MIZU_TIMEOUT);
  /* with the claimant dead the same probe reaps it: DIED, reported by
     position, the other handle collectible */
  mizu_pool_destroy(wk);   /* simulated crash */
  wk = NULL;
  assert(mizu_pool_collect_any(ctrl, ts, 2, &idx, &obj, 1500) == MIZU_ERR);
  assert(idx == 0);
  assert(strcmp(mizu_pool_error(ctrl),
                "worker died while executing this task") == 0);
  assert(mizu_pool_step(wk2, 0) == MIZU_STEP_TASK);
  collect_bytes(ctrl, &t2, "b", 1);

  /* the reaped slot rejoins */
  mizu_binding wb;
  make_binding(&wb, 1);
  assert(mizu_pool_worker_join(&wk, token, 0, &wb) == MIZU_OK);

  /* help mode in collect_any: a worker blocked on its own nested tasks
     pops and executes them rather than parking */
  mizu_bytes b = { (void *) "n", 1 };
  mizu_task n1, n2;
  assert(mizu_pool_submit(wk2, &b, &n1, 0) == MIZU_OK);
  assert(mizu_pool_submit(wk2, &b, &n2, 0) == MIZU_OK);
  exec_calls = 0;
  assert(mizu_pool_collect_any(wk2, &n1, 1, &idx, &obj, 1000) == MIZU_OK);
  assert(idx == 0);
  mizu_bytes_free(obj);
  assert(exec_calls == 2);   /* both nested tasks ran as help */
  collect_bytes(wk2, &n2, "n", 1);

  /* help by steal: own deque empty (its one task is claimed and
     stranded), the peer's deque feeds the steal */
  mizu_task n3;
  assert(mizu_pool_submit(wk2, &b, &n3, 0) == MIZU_OK);   /* wk2's deque */
  mizu_task n4;
  assert(mizu_pool_submit(wk, &b, &n4, 0) == MIZU_OK);    /* wk's deque */
  exec_mode = 1;
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);   /* claims n4, strands */
  exec_mode = 0;
  assert(mizu_pool_collect_all(wk, &n4, 1, vals, &err, &consumed, 60) ==
         MIZU_TIMEOUT);   /* n3 ran as help on the steal */
  collect_bytes(wk2, &n3, "n", 1);
  /* the leave fails the stranded claim and releases it in place: the
     slot is FREE, the sub slot reaps clean */
  assert(mizu_pool_leave(wk) == MIZU_OK);
  assert(mizu_pool_task_state(ctrl, &n4) == MIZU_RS_FREE);
  mizu_pool_destroy(wk);
  wk = NULL;
  pool_end();
  puts("ok vectored_collect");
}

/* collect_all's claim order: the first non-OK by position is the only
   handle consumed; a read failure past a consumed prefix reports the
   prefix. */
static void test_collect_all_errors(void) {
  pool_pair(1, 1, 8, 64, 64, 64, 512);
  mizu_task t1 = submit_bytes(ctrl, "ok", 2);
  mizu_task t2 = submit_bytes(ctrl, "rf", 2);   /* the read fails */
  mizu_task ts[2] = { t1, t2 };
  void *vals[2] = { NULL, NULL };
  size_t err = 9, consumed = 9;
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);
  fail_read = 1;
  assert(mizu_pool_collect_all(ctrl, ts, 2, vals, &err, &consumed,
                               1000) == MIZU_ERR);
  assert(err == 1 && consumed == 1);   /* t1's product handed back */
  mizu_bytes_free(vals[0]);
  fail_read = 0;
  collect_bytes(ctrl, &t2, "rf", 2);   /* retryable: still collectible */

  /* an ERR outcome at position 0: only it is consumed */
  exec_mode = 4;
  mizu_task t3 = submit_bytes(ctrl, "x", 1);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);
  exec_mode = 0;
  mizu_task t4 = submit_bytes(ctrl, "z", 1);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);
  mizu_task ts2[2] = { t3, t4 };
  vals[0] = vals[1] = NULL;
  assert(mizu_pool_collect_all(ctrl, ts2, 2, vals, &err, &consumed,
                               1000) == MIZU_ERR);
  assert(err == 0 && consumed == 0);
  collect_bytes(ctrl, &t4, "z", 1);    /* t4 stayed collectible */
  pool_end();
  puts("ok collect_all_errors");
}

/* A cancelled-between-claim-and-publish task: the publish CAS loses to
   CANCEL, the staged result is discarded, the submitter probe runs. */
static void test_cancel_at_publish(void) {
  pool_pair(1, 1, 8, 64, 64, 64, 512);
  size_t n = 100 * 1024;
  unsigned char *big = malloc(n);
  for (size_t i = 0; i < n; i++) big[i] = (unsigned char) (i * 31u);
  exec_mode = 6;
  cur_task = submit_bytes(ctrl, big, n);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);   /* cancels itself */
  exec_mode = 0;
  /* the publish lost to the cancel: the slot is FREE outright (the
     worker's publish-tail consume), not CANCEL */
  assert(mizu_pool_task_state(ctrl, &cur_task) == MIZU_RS_FREE);
  void *obj = NULL;
  assert(mizu_pool_collect(ctrl, &cur_task, &obj, 0) == MIZU_ERR);
  assert(strcmp(mizu_pool_error(ctrl), "task handle already collected") ==
         0);
  free(big);
  pool_end();
  puts("ok cancel_at_publish");
}

/* A stop with a worker still live: the interrupt hook abandons the wait
   but the teardown completes. */
static int stop_intr_check(void *ctx) {
  (void) ctx;
  static int calls;
  return ++calls > 2;
}

static void test_stop_interrupt(void) {
  mizu_binding b, wb;
  make_binding(&b, 0);
  make_binding(&wb, 1);
  b.check = stop_intr_check;
  mizu_pool_opts opts;
  mizu_pool_opts_init(&opts);
  opts.max_workers = 1;
  opts.max_submitters = 8;
  opts.injection_cap = 64;
  opts.per_worker_cap = 64;
  opts.result_slots = 64;
  opts.slot_size = 512;
  assert(mizu_pool_create(&ctrl, &opts, &b) == MIZU_OK);
  assert(mizu_pool_token(ctrl, token, sizeof(token)) == MIZU_OK);
  assert(mizu_pool_worker_join(&wk, token, 0, &wb) == MIZU_OK);

  /* the worker never observes the shutdown: the wait would park
     forever; the hook abandons it, the teardown still completes */
  assert(mizu_pool_stop(ctrl, -1) == MIZU_ERR);
  assert(mizu_pool_errcat(ctrl) == MIZU_ERRCAT_INTERRUPTED);
  /* the pool is down: submits refuse, the worker's step exits */
  mizu_bytes x = { (void *) "x", 1 };
  mizu_task t;
  assert(mizu_pool_submit(ctrl, &x, &t, 0) == MIZU_ERR);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_SHUTDOWN);
  assert(mizu_pool_leave(wk) == MIZU_OK);
  mizu_pool_destroy(wk);
  wk = NULL;
  mizu_pool_destroy(ctrl);
  ctrl = NULL;
  puts("ok stop_interrupt");
}

/* map_caps: the worker's claim fails when the submitter registry is
   full; a stopped pool refuses. */
static void test_map_caps_errors(void) {
  pool_pair(1, 1, 1, 64, 64, 64, 512);   /* ctrl owns the only sub slot */
  uint32_t free_rs = 9, inj = 9, inl = 9;
  assert(mizu_pool_map_caps(wk, &free_rs, &inj, &inl) == -1);
  assert(strstr(mizu_pool_error(wk), "submitter registry full") != NULL);
  /* a nested submit fails the same way */
  mizu_bytes b = { (void *) "x", 1 };
  mizu_task t;
  assert(mizu_pool_submit(wk, &b, &t, 0) == MIZU_ERR);

  /* a stop with the worker still live times out, but the teardown
     runs — and the worker then reads the shutdown word */
  assert(mizu_pool_stop(ctrl, 100) == MIZU_TIMEOUT);
  assert(mizu_pool_map_caps(wk, &free_rs, &inj, &inl) == -1);
  assert(mizu_pool_errcat(wk) == MIZU_ERRCAT_STOPPED);
  mizu_task t2;
  assert(mizu_pool_submit_batch_fn(wk, NULL, NULL, 0, &t2, &(size_t) { 0 },
                                   0) == MIZU_ERR);
  assert(mizu_pool_errcat(wk) == MIZU_ERRCAT_STOPPED);
  mizu_pool_destroy(wk);
  wk = NULL;
  mizu_pool_destroy(ctrl);
  ctrl = NULL;
  puts("ok map_caps_errors");
}

/* submit_batch_fn: the worker's nested loop, the bad-argument refuses,
   and a doorbell left set while entries remain. */
static void test_batch_nested(void) {
  pool_pair(1, 1, 8, 64, 64, 64, 512);
  mizu_bytes b1 = { (void *) "a", 1 }, b2 = { (void *) "b", 1 };
  void *objs[2] = { &b1, &b2 };
  mizu_task ts[2];
  size_t n_out = 0;
  /* the worker's batch form: nested submits onto its own deque */
  assert(mizu_pool_submit_batch(wk, objs, 2, ts, &n_out, 0) == MIZU_OK);
  assert(n_out == 2);
  exec_calls = 0;
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);
  assert(exec_calls == 2);
  collect_bytes(wk, &ts[0], "a", 1);
  collect_bytes(wk, &ts[1], "b", 1);

  /* help_once with more work queued than claimed restores the doorbell */
  mizu_task t1 = submit_bytes(ctrl, "x", 1);
  mizu_task t2 = submit_bytes(ctrl, "y", 1);
  exec_calls = 0;
  assert(mizu_pool_help_once(wk) == 1);
  assert(exec_calls == 1);
  mizu_pool_dump d;
  assert(mizu_pool_dump_get(ctrl, &d) == MIZU_OK);
  assert(d.help_wanted == 1);   /* an entry remains: the bell rings on */
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);
  collect_bytes(ctrl, &t1, "x", 1);
  collect_bytes(ctrl, &t2, "y", 1);
  pool_end();

  /* the pull caps at a full deque: deque_cap 2 pulls 2 of 3 */
  pool_pair(1, 1, 8, 64, 2, 64, 512);
  mizu_task r1 = submit_bytes(ctrl, "q", 1);
  mizu_task r2 = submit_bytes(ctrl, "r", 1);
  mizu_task r3 = submit_bytes(ctrl, "s", 1);
  assert(mizu_pool_deque_pull(wk, 3) == 2);   /* full: the ring keeps 1 */
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);   /* pops */
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);   /* pops */
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);   /* claims the ring */
  collect_bytes(ctrl, &r1, "q", 1);
  collect_bytes(ctrl, &r2, "r", 1);
  collect_bytes(ctrl, &r3, "s", 1);
  pool_end();
  puts("ok batch_nested");
}

/* The keeper record follows a reused slot's new incarnation, and a
   mid-table removal swaps the tail in. */
static void test_rk_reuse(void) {
  pool_pair(1, 1, 2, 64, 64, 2, 512);   /* ctrl: one result slot */
  size_t n = 100 * 1024;
  unsigned char *big = malloc(n);
  for (size_t i = 0; i < n; i++) big[i] = (unsigned char) (i * 17u);

  mizu_task t1 = submit_bytes(ctrl, big, n);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);   /* kept: rk[0] */
  collect_bytes(ctrl, &t1, big, n);                  /* slot FREE, rk held */
  mizu_task t2 = submit_bytes(ctrl, big, n);         /* the same slot */
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);   /* record follows */
  collect_bytes(ctrl, &t2, big, n);
  free(big);
  pool_end();

  /* two kept results: collecting the first swaps the second's record
     into the vacated slot at the worker's reap */
  pool_pair(1, 1, 2, 64, 64, 4, 512);   /* ctrl: two result slots */
  size_t m = 100 * 1024;
  unsigned char *bg = malloc(m);
  for (size_t i = 0; i < m; i++) bg[i] = (unsigned char) (i * 13u);
  mizu_task u1 = submit_bytes(ctrl, bg, m);
  mizu_task u2 = submit_bytes(ctrl, bg, m);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);
  collect_bytes(ctrl, &u1, bg, m);   /* frees slot 1st: the swap runs at
                                        the worker's next reap */
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_IDLE);
  collect_bytes(ctrl, &u2, bg, m);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_IDLE);
  free(bg);
  pool_end();
  puts("ok rk_reuse");
}

#if !defined(_WIN32)
/* Cross-thread waits: the full-ring space wait unparked by the claim's
   backpressure wake, by the stop broadcast; a parked collect unparked
   by the broadcast's cancel sweep. */
struct wait_ctx {
  mizu_pool *p;
  mizu_bytes b;
  mizu_task t;
  mizu_task ct;
  mizu_status st;
  void *obj;
};

static void *full_submit_main(void *arg) {
  struct wait_ctx *c = arg;
  c->st = mizu_pool_submit(c->p, &c->b, &c->t, -1);
  return NULL;
}

static void *parked_collect_main(void *arg) {
  struct wait_ctx *c = arg;
  c->st = mizu_pool_collect(c->p, &c->ct, &c->obj, -1);
  return NULL;
}

static void test_cross_thread_waits(void) {
  /* the space wait: the worker's claim wakes it */
  pool_pair(1, 1, 8, 2, 64, 64, 512);   /* injection_cap 2 */
  mizu_task t1 = submit_bytes(ctrl, "a", 1);
  mizu_task t2 = submit_bytes(ctrl, "b", 1);
  (void) t2;
  struct wait_ctx c;
  memset(&c, 0, sizeof c);
  c.p = ctrl;
  c.b = (mizu_bytes) { (void *) "c", 1 };
  pthread_t th;
  assert(pthread_create(&th, NULL, full_submit_main, &c) == 0);
  struct timespec ts = { 0, 100000000 };   /* 100 ms: parked by now */
  nanosleep(&ts, NULL);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);   /* claims, wakes */
  assert(pthread_join(th, NULL) == 0);
  assert(c.st == MIZU_OK);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);
  collect_bytes(ctrl, &t1, "a", 1);
  collect_bytes(ctrl, &t2, "b", 1);
  collect_bytes(ctrl, &c.t, "c", 1);
  pool_end();

  /* the stop broadcast wakes a full waiter with STOPPED */
  pool_pair(1, 1, 8, 2, 64, 64, 512);
  t1 = submit_bytes(ctrl, "a", 1);
  t2 = submit_bytes(ctrl, "b", 1);
  (void) t1;
  (void) t2;
  memset(&c, 0, sizeof c);
  c.p = ctrl;
  c.b = (mizu_bytes) { (void *) "c", 1 };
  assert(pthread_create(&th, NULL, full_submit_main, &c) == 0);
  nanosleep(&ts, NULL);
  assert(mizu_pool_stop(ctrl, 500) == MIZU_TIMEOUT);   /* wk never exits */
  assert(pthread_join(th, NULL) == 0);
  assert(c.st == MIZU_ERR);
  mizu_pool_destroy(wk);
  wk = NULL;
  mizu_pool_destroy(ctrl);
  ctrl = NULL;

  /* the stop broadcast's cancel sweep wakes a parked collect */
  pool_pair(1, 1, 2, 64, 64, 64, 512);
  mizu_binding sb;
  make_binding(&sb, 0);
  assert(mizu_pool_attach(&sub, token, &sb) == MIZU_OK);
  memset(&c, 0, sizeof c);
  c.p = sub;
  c.b = (mizu_bytes) { (void *) "doomed", 6 };
  assert(mizu_pool_submit(sub, &c.b, &c.ct, 0) == MIZU_OK);
  assert(pthread_create(&th, NULL, parked_collect_main, &c) == 0);
  nanosleep(&ts, NULL);
  assert(mizu_pool_stop(ctrl, 500) == MIZU_TIMEOUT);   /* wk never exits */
  assert(pthread_join(th, NULL) == 0);
  assert(c.st == MIZU_ERR);
  assert(strcmp(mizu_pool_error(sub), "task cancelled or pool stopped") ==
         0);
  mizu_pool_destroy(sub);
  sub = NULL;
  mizu_pool_destroy(wk);
  wk = NULL;
  mizu_pool_destroy(ctrl);
  ctrl = NULL;
  puts("ok cross_thread_waits");
}

/* The forked-child teardown: the fork guard refuses the verbs, then
   abandon frees only the child's copies — the parent's pool is
   unharmed. */
static void test_abandon_fork(void) {
  pool_pair(1, 1, 8, 64, 64, 64, 512);
  pid_t pid = fork();
  assert(pid >= 0);
  if (pid == 0) {
    mizu_bytes b = { (void *) "x", 1 };
    mizu_task t;
    if (mizu_pool_submit(ctrl, &b, &t, 0) != MIZU_ERR ||
        strstr(mizu_pool_error(ctrl), "do not survive fork") == NULL)
      exit(1);
    mizu_pool_abandon(wk);
    mizu_pool_abandon(ctrl);
    exit(0);
  }
  int st;
  assert(waitpid(pid, &st, 0) == pid);
  assert(WIFEXITED(st) && WEXITSTATUS(st) == 0);
  mizu_task t = submit_bytes(ctrl, "alive", 5);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);
  collect_bytes(ctrl, &t, "alive", 5);
  pool_end();
  puts("ok abandon_fork");
}
#endif

/* The announced-but-not-yet-parked wake: a pusher finding a worker in
   WPK_IDLE unparks it (the epoch bump turns its upcoming sleep into an
   immediate return). Wire state set up directly — the window is a race
   no single-threaded sequence can schedule. */
static void test_unpark_idle(void) {
  pool_pair(1, 1, 8, 64, 64, 64, 512);
  char name[80];
  int nl = snprintf(name, sizeof(name), MIZU_PREFIX_LITERAL "%s", token);
  assert(nl > 0 && (size_t) nl < sizeof(name));
  mizu_shm *shm = NULL;
  assert(mizu_shm_open_rw(&shm, name, 0) == MIZU_OK);
  unsigned char *base = mizu_shm_addr(shm);
  mizu_wk_slot *w0 = (mizu_wk_slot *) (base + 64);
  mizu_pool_hdr h;
  memcpy(&h, base, sizeof(h));
  size_t ctrl_off = 64 + (size_t) h.max_workers * sizeof(mizu_wk_slot) +
    (size_t) h.max_submitters * sizeof(mizu_sub_slot) + 128 +
    (size_t) h.max_submitters *
      (MIZU_INJ_META_SIZE + (size_t) h.inj_cap * h.slot) +
    (size_t) h.max_workers * ((size_t) h.deque_cap * h.slot) +
    (size_t) h.result_slots * h.slot;
  _Atomic uint64_t *parked =
    (_Atomic uint64_t *) (base + ctrl_off + MIZU_CTRL_PARKED_OFF);

  /* announce as the worker would mid-transition: IDLE + the mask bit */
  atomic_store(&w0->park_state, MIZU_WPK_IDLE);
  uint32_t epoch_before = atomic_load(&w0->park_epoch);
  atomic_fetch_or(parked, 1ull);
  mizu_task t = submit_bytes(ctrl, "x", 1);   /* the pusher wakes it */
  assert(atomic_load(&w0->park_epoch) > epoch_before);
  /* restore: the step must see a clean, running slot */
  atomic_fetch_and(parked, ~1ull);
  atomic_store(&w0->park_state, MIZU_WPK_RUNNING);
  assert(mizu_pool_step(wk, 0) == MIZU_STEP_TASK);
  collect_bytes(ctrl, &t, "x", 1);
  mizu_shm_close(shm, 0);
  pool_end();
  puts("ok unpark_idle");
}

int main(void) {
  mizu_binding_bytes(&bytesb);

  test_create_validate();
  test_hdr_validate();
  test_handle_guards();
  test_join_errors();
  test_nested_no_exec();
  test_stage_failure();
  test_vectored_collect();
  test_collect_all_errors();
  test_cancel_at_publish();
  test_stop_interrupt();
  test_map_caps_errors();
  test_batch_nested();
  test_rk_reuse();
#if !defined(_WIN32)
  test_cross_thread_waits();
  test_abandon_fork();
#endif
  test_unpark_idle();
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
  test_leave_fails_in_flight();
  test_leave_fails_executing();
  test_unwind();
  test_retire_lame_duck();
  test_orphan_drain();
  test_submitter_reap();
  test_sub_slot_recycle();
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
  test_ledger_key_kinds();
  test_lost_task();
#if !defined(_WIN32)
  test_zombie_entry();
#endif

  puts("test_pool: all passed");
  return 0;
}
