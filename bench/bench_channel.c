/* Bench: channel round-trip latency across payload tiers, batch
   throughput, parker wake latency, and the spill free-list recycle vs
   fresh-create cost. Report-only (no timing asserts — runner timing is
   too variable); records are appended to bench/notes.md by hand with
   the commit SHA. POSIX (pthread); a stub elsewhere. `make bench`. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

#ifdef _WIN32

int main(void) {
  puts("bench_channel: skipped on Windows (pthread-based)");
  return 0;
}

#else

#include <pthread.h>
#include <time.h>

static mizu_channel *host, *peer;
static _Atomic int stop_flag;
static _Atomic uint64_t drained;

static void *echo_main(void *arg) {
  (void) arg;
  while (!stop_flag) {
    void *obj = NULL;
    mizu_status st = mizu_channel_recv(peer, &obj, 50);
    if (st == MIZU_OK) {
      mizu_status st2 = mizu_channel_send(peer, obj);
      while (st2 == MIZU_FULL && !stop_flag)      /* ring full: retry */
        st2 = mizu_channel_send(peer, obj);
      mizu_bytes_free(obj);
    }
  }
  return NULL;
}

static void *drain_main(void *arg) {
  (void) arg;
  void *objs[64];
  while (!stop_flag) {
    size_t n = 0;
    mizu_status st = mizu_channel_recv_batch(peer, objs, 64, &n, 50);
    if (st != MIZU_OK) continue;
    for (size_t i = 0; i < n; i++) mizu_bytes_free(objs[i]);
    drained += n;
  }
  return NULL;
}

static int cmp_double(const void *a, const void *b) {
  double d = *(const double *) a - *(const double *) b;
  return (d > 0) - (d < 0);
}

/* median of the recorded per-iteration samples */
static double median(double *s, size_t n) {
  qsort(s, n, sizeof(double), cmp_double);
  return s[n / 2];
}

static void channel_pair(void) {
  mizu_binding b;
  mizu_binding_bytes(&b);
  mizu_channel_opts opts;
  mizu_channel_opts_init(&opts);
  opts.capacity = 4096;
  opts.slot_size = 256;
  opts.arena_size = 8u << 20;
  if (mizu_channel_create(&host, &opts, &b) != MIZU_OK) exit(1);
  char token[64];
  if (mizu_channel_token(host, token, sizeof(token)) != MIZU_OK) exit(1);
  if (mizu_channel_attach(&peer, token, &b) != MIZU_OK) exit(1);
  if (mizu_channel_ready_set(peer) != MIZU_OK) exit(1);
  if (mizu_channel_ready_wait(host, 30000) != MIZU_OK) exit(1);
}

static void channel_end(void) {
  mizu_channel_destroy(host);
  mizu_channel_destroy(peer);
}

/* Round-trip latency at one payload size: send, wait the echo. */
static void bench_roundtrip(const char *name, size_t len, size_t n) {
  uint8_t *buf = malloc(len != 0 ? len : 1);
  memset(buf, 0x5A, len);
  double *samples = malloc(n * sizeof(double));
  mizu_bytes b = { buf, len };

  size_t warm = n / 10 + 10;
  for (size_t i = 0; i < warm; i++) {
    if (mizu_channel_send(host, &b) != MIZU_OK) exit(1);
    void *obj = NULL;
    if (mizu_channel_recv(host, &obj, 30000) != MIZU_OK) exit(1);
    mizu_bytes_free(obj);
  }
  for (size_t i = 0; i < n; i++) {
    double t0 = mizu_now();
    if (mizu_channel_send(host, &b) != MIZU_OK) exit(1);
    void *obj = NULL;
    if (mizu_channel_recv(host, &obj, 30000) != MIZU_OK) exit(1);
    double t1 = mizu_now();
    mizu_bytes_free(obj);
    samples[i] = (t1 - t0) * 1e6;
  }
  printf("channel_roundtrip_%-6s %8.2f us  (median, n=%zu)\n", name,
         median(samples, n), n);
  free(samples);
  free(buf);
}

static void bench_batch(void) {
  enum { TOTAL = 1000000, BATCH = 32 };
  uint8_t buf[64];
  memset(buf, 0x5A, sizeof(buf));
  mizu_bytes parts[BATCH];
  void *objs[BATCH];
  for (int i = 0; i < BATCH; i++) {
    parts[i].data = buf;
    parts[i].len = sizeof(buf);
    objs[i] = &parts[i];
  }
  drained = 0;
  double t0 = mizu_now();
  int sent = 0;
  while (sent < TOTAL) {
    size_t acc = 0;
    if (mizu_channel_send_batch(host, objs, BATCH, &acc) != MIZU_OK) exit(1);
    sent += (int) acc;
  }
  while (drained < (uint64_t) TOTAL) {           /* the peer drains */
    if (mizu_now() - t0 > 60) exit(1);
  }
  double dt = mizu_now() - t0;
  printf("channel_batch_64B     %8.2f Mmsg/s  (%d msgs in %.2f s)\n",
         TOTAL / dt / 1e6, TOTAL, dt);
}

/* Parker wake latency: a parked thread, a directed unpark, the
   wake turnaround measured unpark -> park-return. */
struct parker_arg {
  mizu_parker *pk;
  _Atomic uint64_t *wake_at;
};

/* the parker thread parks on the epoch until woken, then acks */
static void *park_main(void *arg) {
  struct parker_arg *a = arg;
  while (!stop_flag) {
    uint32_t snap = mizu_parker_snapshot(a->pk);
    mizu_park(a->pk, snap, 2000);
    atomic_store_explicit(a->wake_at, (uint64_t) 1, memory_order_release);
  }
  return NULL;
}

static void bench_parker(void) {
  _Atomic uint32_t epoch = 0;
  _Atomic uint64_t wake_at = 0;
  mizu_parker pk;
  if (mizu_parker_attach(&pk, &epoch, NULL, 0, 1) != 0) exit(1);

  pthread_t th;
  struct parker_arg arg = { &pk, &wake_at };
  stop_flag = 0;
  if (pthread_create(&th, NULL, park_main, &arg) != 0) exit(1);

  enum { N = 2000 };
  double *samples = malloc(N * sizeof(double));
  struct timespec ts = { 0, 1000000 };   /* 1 ms: let the thread park */
  for (int i = 0; i < N; i++) {
    nanosleep(&ts, NULL);
    atomic_store_explicit(&wake_at, 0, memory_order_release);
    double t0 = mizu_now();
    mizu_unpark(&pk);
    while (atomic_load_explicit(&wake_at, memory_order_acquire) == 0)
      MIZU_PAUSE();
    samples[i] = (mizu_now() - t0) * 1e6;
  }
  stop_flag = 1;
  mizu_unpark(&pk);
  pthread_join(th, NULL);
  mizu_parker_detach(&pk);
  printf("parker_wake           %8.2f us  (median, n=%d)\n",
         median(samples, N), N);
  free(samples);
}

/* Spill free-list recycle vs fresh region create, 1 MiB class. */
static void bench_spill(void) {
  enum { FRESH_N = 300, RECYCLE_N = 100000 };
  double t0 = mizu_now();
  for (int i = 0; i < FRESH_N; i++) {
    mizu_shm *shm = NULL;
    if (mizu_shm_create(&shm, 1u << 20) != MIZU_OK) exit(1);
    mizu_shm_close(shm, 1);
  }
  double fresh = (mizu_now() - t0) / FRESH_N * 1e6;

  mizu_spill_fl fl;
  memset(&fl, 0, sizeof(fl));
  mizu_shm *shm = NULL;
  if (mizu_shm_create(&shm, 1u << 20) != MIZU_OK) exit(1);
  mizu_spill_fl_insert(&fl, shm);
  t0 = mizu_now();
  for (int i = 0; i < RECYCLE_N; i++) {
    mizu_shm *s = mizu_spill_region_get(&fl, 1u << 20);
    fl.staging = NULL;   /* no retain machinery in play here */
    mizu_spill_fl_insert(&fl, s);
  }
  double recycle = (mizu_now() - t0) / RECYCLE_N * 1e6;
  mizu_spill_fl_teardown(&fl);
  printf("spill_fresh_1MiB      %8.2f us  per create+close (n=%d)\n",
         fresh, FRESH_N);
  printf("spill_recycle_1MiB    %8.2f us  per pop+insert (n=%d)\n",
         recycle, RECYCLE_N);
}

int main(void) {
  channel_pair();
  stop_flag = 0;
  pthread_t echo;
  if (pthread_create(&echo, NULL, echo_main, NULL) != 0) exit(1);
  bench_roundtrip("NIL", 0, 50000);
  bench_roundtrip("64B", 64, 50000);
  bench_roundtrip("64KiB", 64u << 10, 10000);
  bench_roundtrip("1MiB", 1u << 20, 3000);
  stop_flag = 1;
  pthread_join(echo, NULL);

  stop_flag = 0;
  pthread_t drain;
  if (pthread_create(&drain, NULL, drain_main, NULL) != 0) exit(1);
  bench_batch();
  stop_flag = 1;
  pthread_join(drain, NULL);
  channel_end();

  bench_parker();
  bench_spill();
  return 0;
}

#endif
