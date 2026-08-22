/* Soak tier: pool contention for REI_SOAK_SECONDS (the make default is
   120; nightly runs set it higher) — forked workers running the echo
   exec and forked submitters keeping a window of sequence-tagged tasks
   outstanding around one controller. Tasks ride the INLINE and SHM_RAW
   spill tiers; collects go through collect_any with a cancel mixed in;
   every echoed payload must match its task's sequence tag and fill
   pattern. Ends with a full collect_all drain, a clean stop, and
   exit-code checks. POSIX (fork); a stub passes elsewhere. Run via
   `make test-soak`. */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

#ifdef _WIN32

int main(void) {
  puts("soak_pool: skipped on Windows (fork-based)");
  return 0;
}

#else

#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_PAYLOAD (512u << 10)
#define WIN_CAP 128
#define N_WORKERS 4
#define N_SUBS 2

static char pool_token[64];
static double run_seconds;
static uint64_t rng_state;

static uint64_t rnd(void) {
  uint64_t x = rng_state;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  rng_state = x;
  return x * 0x2545F4914F6CDD1DULL;
}

/* 10% NIL, 55% INLINE (8-32 B), 25% spill (512 B-4 KiB), 10% big spill
   (100-512 KiB). Seq-bearing payloads are always >= 8 bytes. */
static size_t pick_len(void) {
  uint64_t r = rnd() % 100;
  if (r < 10) return 0;
  if (r < 65) return 8 + rnd() % 25;
  if (r < 90) return 512 + rnd() % 3585;
  return (100 + rnd() % 413) << 10;
}

/* bytes [0-7]: the task's sequence number, LE; the rest its low byte */
static void fill(uint8_t *buf, size_t len, uint64_t seq) {
  for (int i = 0; i < 8; i++) buf[i] = (uint8_t) (seq >> (8 * i));
  memset(buf + 8, (uint8_t) seq, len - 8);
}

static void verify(const rei_bytes *b, size_t len, uint64_t seq) {
  assert(b->len == len);
  if (len == 0) return;
  const uint8_t *d = b->data;
  uint64_t got = 0;
  for (int i = 0; i < 8; i++) got |= (uint64_t) d[i] << (8 * i);
  assert(got == seq);
  for (size_t i = 8; i < len; i++) assert(d[i] == (uint8_t) seq);
}

// Worker child ------------------------------------------------------------------

/* The echo evaluator: NIL -> empty, INLINE -> the payload bytes,
   SHM_RAW -> the spill region's stream (resolved through the worker
   handle's open cache; the borrowed mapping stays valid until the
   publish, the task's consumer-done point). */
static int soak_exec(const rei_slot_hdr *hdr, const uint8_t *payload,
                     size_t limit, rei_result_sink *sink, int catching,
                     void *ctx) {
  (void) limit; (void) catching; (void) ctx;
  rei_bytes b;
  if (hdr->kind == REI_KIND_NIL) {
    b.data = NULL;
    b.len = 0;
  } else if (hdr->kind == REI_KIND_INLINE) {
    b.data = (void *) payload;
    b.len = hdr->len;
  } else {
    assert(hdr->kind == REI_KIND_SHM_RAW);
    rei_read_ctx rctx;
    memset(&rctx, 0, sizeof(rctx));
    rctx.size = (uint32_t) sizeof(rctx);
    rctx.outcome = REI_RS_OK;
    rctx.died_slot = -1;
    rctx.handle = (rei_handle *) sink->p;   /* the handle base is first */
    rei_shm *shm = rei_read_region(&rctx, payload, hdr->len);
    assert(shm != NULL && hdr->aux <= (uint64_t) shm->size);
    b.data = shm->addr;
    b.len = (size_t) hdr->aux;
  }
  return rei_result_publish(sink, &b) >= 0 ? 0 : 1;
}

static void run_worker(uint32_t slot) {
  rei_binding b;
  rei_binding_bytes(&b);
  b.exec = soak_exec;
  rei_pool *p;
  if (rei_pool_worker_join(&p, pool_token, slot, &b) != REI_OK) _exit(3);
  if (rei_pool_worker_run(p) != REI_EXIT_SHUTDOWN) _exit(4);
  if (rei_pool_leave(p) != REI_OK) _exit(5);
  rei_pool_destroy(p);
  _exit(0);
}

// Submitter child -----------------------------------------------------------------

/* A window of outstanding tasks; removal swaps the tail in (collect_any
   ties break to the earliest position regardless). */
struct window {
  rei_task task[WIN_CAP];
  uint64_t seq[WIN_CAP];
  size_t len[WIN_CAP];
  size_t n;
};

static void win_remove(struct window *w, size_t i) {
  w->n--;
  w->task[i] = w->task[w->n];
  w->seq[i] = w->seq[w->n];
  w->len[i] = w->len[w->n];
}

static void run_submitter(void) {
  rei_binding b;
  rei_binding_bytes(&b);
  rei_pool *p;
  if (rei_pool_attach(&p, pool_token, &b) != REI_OK) _exit(3);

  struct window w;
  w.n = 0;
  uint8_t *buf = malloc(MAX_PAYLOAD);
  uint64_t next_seq = 1;
  uint64_t submitted = 0, collected = 0, cancelled = 0;
  double deadline = rei_now() + run_seconds;

  while (rei_now() < deadline) {
    if (w.n < WIN_CAP) {
      size_t len = pick_len();
      if (len != 0) fill(buf, len, next_seq);
      rei_bytes tb = { buf, len };
      rei_task t;
      rei_status st = rei_pool_submit(p, &tb, &t, 250);
      if (st == REI_OK) {
        w.task[w.n] = t;
        w.seq[w.n] = next_seq;
        w.len[w.n] = len;
        w.n++;
        submitted++;
        if (len != 0) next_seq++;
      } else {
        assert(st == REI_FULL);   /* ring back-pressure at the deadline */
      }
    }
    if (w.n != 0 && rnd() % 64 == 0) {
      size_t i = rnd() % w.n;
      if (rei_pool_cancel(p, &w.task[i]) == 1) {
        win_remove(&w, i);
        cancelled++;
      }   /* 0: already running/terminal — it collects normally */
    }
    if (w.n != 0) {
      size_t idx = 0;
      void *obj = NULL;
      rei_status st = rei_pool_collect_any(p, w.task, w.n, &idx, &obj, 100);
      if (st == REI_OK) {
        rei_bytes *rb = obj;
        assert(rb != NULL);
        verify(rb, w.len[idx], w.seq[idx]);
        rei_bytes_free(rb);
        win_remove(&w, idx);
        collected++;
      } else {
        assert(st == REI_TIMEOUT);
      }
    }
  }

  /* drain: every outstanding task completes and verifies */
  if (w.n != 0) {
    void **vals = calloc(w.n, sizeof(void *));
    size_t err = 0;
    assert(rei_pool_collect_all(p, w.task, w.n, vals, &err, 60000) == REI_OK);
    assert(err == w.n);
    for (size_t i = 0; i < w.n; i++) {
      rei_bytes *rb = vals[i];
      assert(rb != NULL);
      verify(rb, w.len[i], w.seq[i]);
      rei_bytes_free(rb);
      collected++;
    }
    free(vals);
  }
  assert(submitted == collected + cancelled);
  free(buf);
  rei_pool_destroy(p);
  _exit(0);
}

// Controller ---------------------------------------------------------------------

int main(void) {
  const char *env = getenv("REI_SOAK_SECONDS");
  run_seconds = env != NULL ? atof(env) : 120;
  assert(run_seconds > 0);

  rei_pool_opts opts;
  rei_pool_opts_init(&opts);
  opts.max_workers = N_WORKERS;
  opts.max_submitters = 1 + N_SUBS;
  opts.injection_cap = 256;
  opts.per_worker_cap = 256;
  opts.result_slots = 512 * (1 + N_SUBS);
  opts.slot_size = 512;
  rei_binding b;
  rei_binding_bytes(&b);
  rei_pool *ctrl;
  assert(rei_pool_create(&ctrl, &opts, &b) == REI_OK);
  assert(rei_pool_token(ctrl, pool_token, sizeof(pool_token)) == REI_OK);

  pid_t pids[N_WORKERS + N_SUBS];
  fflush(stdout);
  for (uint32_t i = 0; i < N_WORKERS; i++) {
    pids[i] = fork();
    assert(pids[i] >= 0);
    if (pids[i] == 0) run_worker(i);
  }
  uint32_t slots[N_WORKERS] = { 0, 1, 2, 3 };
  assert(rei_pool_ready_wait(ctrl, slots, N_WORKERS, 30000) == REI_OK);
  for (uint32_t j = 0; j < N_SUBS; j++) {
    pids[N_WORKERS + j] = fork();
    assert(pids[N_WORKERS + j] >= 0);
    if (pids[N_WORKERS + j] == 0) {
      rng_state = 0x100000001B3ULL * (j + 1);
      run_submitter();
    }
  }

  /* a hung child fails loudly rather than blocking the nightly */
  alarm((unsigned int) run_seconds + 180);
  /* submitters exit at their deadline; only then does stop release the
     workers' runs */
  for (int j = 0; j < N_SUBS; j++) {
    int st = 0;
    assert(waitpid(pids[N_WORKERS + j], &st, 0) == pids[N_WORKERS + j]);
    assert(WIFEXITED(st) && WEXITSTATUS(st) == 0);
  }
  assert(rei_pool_stop(ctrl, 30000) == REI_OK);
  for (int i = 0; i < N_WORKERS; i++) {
    int st = 0;
    assert(waitpid(pids[i], &st, 0) == pids[i]);
    assert(WIFEXITED(st) && WEXITSTATUS(st) == 0);
  }
  rei_pool_destroy(ctrl);
  printf("soak_pool: ok (%d workers x %d submitters, %.0f s)\n",
         N_WORKERS, N_SUBS, run_seconds);
  return 0;
}

#endif
