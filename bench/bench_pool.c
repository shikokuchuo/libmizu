/* Bench: pool submit-collect round-trip latency and steal scaling vs
   worker count. In-process workers on pthreads (in-process joins skip
   the death watches — fine for timing). Report-only (no timing
   asserts); records are appended to bench/notes.md by hand with the
   commit SHA. POSIX (pthread); a stub elsewhere. `make bench`. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

#ifdef _WIN32

int main(void) {
  puts("bench_pool: skipped on Windows (pthread-based)");
  return 0;
}

#else

#include <pthread.h>

/* The echo evaluator: INLINE/NIL tasks echo their bytes; SHM_RAW
   resolves through the worker handle's open cache (the borrowed mapping
   is valid until the publish, the task's consumer-done point). */
static int bench_exec(const mizu_slot_hdr *hdr, const uint8_t *payload,
                      size_t limit, mizu_result_sink *sink, int catching,
                      mizu_read_ctx *ctx) {
  (void) limit; (void) catching; (void) ctx;
  mizu_bytes b;
  if (hdr->kind == MIZU_KIND_NIL) {
    b.data = NULL;
    b.len = 0;
  } else if (hdr->kind == MIZU_KIND_INLINE) {
    b.data = (void *) payload;
    b.len = hdr->len;
  } else {
    mizu_read_ctx rctx;
    memset(&rctx, 0, sizeof(rctx));
    rctx.size = (uint32_t) sizeof(rctx);
    rctx.outcome = MIZU_RS_OK;
    rctx.died_slot = -1;
    rctx.handle = (mizu_handle *) sink->p;   /* the handle base is first */
    mizu_shm *shm = mizu_read_region(&rctx, payload, hdr->len);
    if (shm == NULL) return 1;
    b.data = shm->addr;
    b.len = (size_t) hdr->aux;
  }
  return mizu_result_publish(sink, &b) >= 0 ? 0 : 1;
}

struct worker_arg {
  char token[64];
  uint32_t slot;
};

static void *worker_main(void *arg) {
  struct worker_arg *a = arg;
  mizu_binding b;
  mizu_binding_bytes(&b);
  b.exec = bench_exec;
  mizu_pool *p;
  if (mizu_pool_worker_join(&p, a->token, a->slot, &b) != MIZU_OK) exit(1);
  if (mizu_pool_worker_run(p) != MIZU_EXIT_SHUTDOWN) exit(1);
  if (mizu_pool_leave(p) != MIZU_OK) exit(1);
  mizu_pool_destroy(p);
  return NULL;
}

static mizu_pool *make_pool(uint32_t workers, pthread_t *tids,
                           struct worker_arg *args) {
  mizu_pool_opts opts;
  mizu_pool_opts_init(&opts);
  opts.max_workers = workers;
  opts.max_submitters = 2;
  opts.injection_cap = 1024;
  opts.per_worker_cap = 1024;
  opts.result_slots = 8192;
  opts.slot_size = 512;
  mizu_binding b;
  mizu_binding_bytes(&b);
  mizu_pool *ctrl;
  if (mizu_pool_create(&ctrl, &opts, &b) != MIZU_OK) exit(1);
  char token[64];
  if (mizu_pool_token(ctrl, token, sizeof(token)) != MIZU_OK) exit(1);
  uint32_t *slots = malloc(workers * sizeof(uint32_t));
  for (uint32_t i = 0; i < workers; i++) {
    snprintf(args[i].token, sizeof(args[i].token), "%s", token);
    args[i].slot = i;
    slots[i] = i;
    if (pthread_create(&tids[i], NULL, worker_main, &args[i]) != 0)
      exit(1);
  }
  if (mizu_pool_ready_wait(ctrl, slots, workers, 30000) != MIZU_OK) exit(1);
  free(slots);
  return ctrl;
}

static void end_pool(mizu_pool *ctrl, uint32_t workers, pthread_t *tids) {
  if (mizu_pool_stop(ctrl, 30000) != MIZU_OK) exit(1);
  for (uint32_t i = 0; i < workers; i++) pthread_join(tids[i], NULL);
  mizu_pool_destroy(ctrl);
}

static int cmp_double(const void *a, const void *b) {
  double d = *(const double *) a - *(const double *) b;
  return (d > 0) - (d < 0);
}

/* Submit-collect round trip at one payload size, one worker. */
static void bench_latency(const char *name, size_t len, size_t n) {
  pthread_t tids[1];
  struct worker_arg args[1];
  mizu_pool *ctrl = make_pool(1, tids, args);

  uint8_t *buf = malloc(len != 0 ? len : 1);
  memset(buf, 0x5A, len);
  double *samples = malloc(n * sizeof(double));
  mizu_bytes b = { buf, len };

  for (size_t i = 0; i < n / 10 + 10; i++) {   /* warmup */
    mizu_task t;
    if (mizu_pool_submit(ctrl, &b, &t, 30000) != MIZU_OK) exit(1);
    void *obj = NULL;
    if (mizu_pool_collect(ctrl, &t, &obj, 30000) != MIZU_OK) exit(1);
    mizu_bytes_free(obj);
  }
  for (size_t i = 0; i < n; i++) {
    mizu_task t;
    double t0 = mizu_now();
    if (mizu_pool_submit(ctrl, &b, &t, 30000) != MIZU_OK) exit(1);
    void *obj = NULL;
    if (mizu_pool_collect(ctrl, &t, &obj, 30000) != MIZU_OK) exit(1);
    double t1 = mizu_now();
    mizu_bytes_free(obj);
    samples[i] = (t1 - t0) * 1e6;
  }
  qsort(samples, n, sizeof(double), cmp_double);
  printf("pool_submit_collect_%-4s %7.2f us  (median, n=%zu)\n", name,
         samples[n / 2], n);
  free(samples);
  free(buf);
  end_pool(ctrl, 1, tids);
}

/* Steal scaling: a burst of small tasks through W workers, pipelined in
   chunks so submission and execution overlap. */
static void bench_steal(uint32_t workers) {
  enum { TOTAL = 200000, CHUNK = 512 };
  pthread_t tids[4];
  struct worker_arg args[4];
  mizu_pool *ctrl = make_pool(workers, tids, args);

  uint8_t buf[8];
  memset(buf, 0x5A, sizeof(buf));
  mizu_bytes parts[CHUNK];
  void *objs[CHUNK];
  for (int i = 0; i < CHUNK; i++) {
    parts[i].data = buf;
    parts[i].len = sizeof(buf);
    objs[i] = &parts[i];
  }
  mizu_task ring[2][CHUNK];   /* two chunks in flight: submit into one */
  size_t ring_n[2] = { 0, 0 };   /* while collecting the other       */
  int head_slot = 0;             /* the older chunk's slot           */
  int in_flight = 0;
  int submitted = 0, collected = 0;

  double t0 = mizu_now();
  while (collected < TOTAL) {
    if (in_flight == 2 || (submitted == TOTAL && in_flight != 0)) {
      void *vals[CHUNK];
      size_t err = 0;
      if (mizu_pool_collect_all(ctrl, ring[head_slot], ring_n[head_slot],
                               vals, &err, 60000) != MIZU_OK)
        exit(1);
      for (size_t i = 0; i < ring_n[head_slot]; i++) mizu_bytes_free(vals[i]);
      collected += (int) ring_n[head_slot];
      head_slot = 1 - head_slot;
      in_flight--;
    } else {
      size_t want = (size_t) (TOTAL - submitted) < (size_t) CHUNK ?
        (size_t) (TOTAL - submitted) : (size_t) CHUNK;
      int slot = in_flight == 0 ? head_slot : 1 - head_slot;
      size_t n_out = 0;
      if (mizu_pool_submit_batch(ctrl, objs, want, ring[slot], &n_out,
                                60000) != MIZU_OK || n_out != want)
        exit(1);
      ring_n[slot] = n_out;
      submitted += (int) n_out;
      in_flight++;
    }
  }
  double dt = mizu_now() - t0;
  printf("pool_steal_w%u          %7.2f ktask/s  (%d tasks in %.2f s)\n",
         workers, TOTAL / dt / 1e3, TOTAL, dt);
  end_pool(ctrl, workers, tids);
}

int main(void) {
  bench_latency("NIL", 0, 20000);
  bench_latency("4KiB", 4096, 10000);
  bench_steal(1);
  bench_steal(2);
  bench_steal(4);
  return 0;
}

#endif
