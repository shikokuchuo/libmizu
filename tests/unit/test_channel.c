/* Unit tier: the channel transport, exercised in-process as a
   host/peer pair staging through the built-in bytes binding (the
   consumer path), plus deliberately failing bindings for the
   staging-rollback and interrupt-abandon contracts. The held-lock recv
   scenario execs this binary (argv mode "peer") so a threaded parent
   never forks. Assert-based; run via `make test`. */

/* _GNU_SOURCE: glibc hides flock and other POSIX declarations under
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
#include <pthread.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif

#include "internal.h"

static void send_bytes(mizu_channel *c, const void *data, size_t len) {
  mizu_bytes b = { (void *) data, len };
  assert(mizu_channel_send(c, &b) == MIZU_OK);
}

static void array_sink(void *ctx, size_t i, void *obj) {
  ((void **) ctx)[i] = obj;
}

static void recv_bytes(mizu_channel *c, const void *expect, size_t len) {
  void *obj = NULL;
  assert(mizu_channel_recv(c, &obj, 1000) == MIZU_OK);
  mizu_bytes *b = obj;
  assert(b->len == len);
  assert(len == 0 || memcmp(b->data, expect, len) == 0);
  mizu_bytes_free(b);
}

// A modal binding wrapping the bytes stager: pin every send, or fail ----

static mizu_binding bytesb;
static int pin_token;
static int drops;
static int stage_mode;               /* 0 = bytes+pin, 1 = fail, 2 = spill+fail */

static void count_drop(void *ctx, void *pin) {
  (void) ctx;
  assert(pin == &pin_token);
  drops++;
}

static int wrap_stage(void *obj, mizu_slot_hdr *hdr, uint8_t *payload,
                      uint32_t inline_max, mizu_handle *h, void *ctx) {
  if (stage_mode == 1) return 1;
  if (stage_mode == 2) {
    /* abandon after retaining: the checkout must roll back at the
       next verb */
    mizu_shm *shm;
    if (mizu_stage_spill_get(h, 8192, &shm) != MIZU_OK) return 1;
    mizu_stage_retain(h, shm);
    return 1;
  }
  int rc = bytesb.stage(obj, hdr, payload, inline_max, h, ctx);
  if (rc == 0) mizu_stage_pin(h, &pin_token);
  return rc;
}

/* A frame the bytes reader must refuse (a view tier). */
static int stage_shm_vec(void *obj, mizu_slot_hdr *hdr, uint8_t *payload,
                         uint32_t inline_max, mizu_handle *h, void *ctx) {
  (void) obj; (void) h; (void) ctx;
  const char *name = "/mizu_nonexistent";
  hdr->kind = MIZU_KIND_SHM_VEC;
  hdr->len = (uint32_t) strlen(name);
  hdr->aux = 0;
  assert(strlen(name) <= inline_max);
  memcpy(payload, name, strlen(name));
  return 0;
}

static int check_calls;
static int check_intr(void *ctx) {
  (void) ctx;
  return ++check_calls > 2;
}

/* A modal stager: one view-tier frame on demand, bytes otherwise — the
   batch mid-stream read failure needs a good message ahead of a bad
   one. */
static int vec_next;
static int stage_modal_vec(void *obj, mizu_slot_hdr *hdr, uint8_t *payload,
                           uint32_t inline_max, mizu_handle *h, void *ctx) {
  if (vec_next) return stage_shm_vec(obj, hdr, payload, inline_max, h, ctx);
  return bytesb.stage(obj, hdr, payload, inline_max, h, ctx);
}

/* A hand-crafted RAWSPILL frame: offset 0 reads the arena head (the
   arena is fresh — zero bytes); mode 1 points past it (corrupt). */
static int rawspill_mode;
static int stage_rawspill(void *obj, mizu_slot_hdr *hdr, uint8_t *payload,
                          uint32_t inline_max, mizu_handle *h, void *ctx) {
  (void) obj; (void) h; (void) ctx; (void) inline_max;
  hdr->kind = MIZU_KIND_RAWSPILL;
  uint64_t off = rawspill_mode ? (uint64_t) 1 << 20 : 0;
  memcpy(payload, &off, sizeof(off));
  hdr->len = 8;
  hdr->aux = 0;
  return 0;
}

#ifndef _WIN32
/* The gone-peer child (exec'd): attach, rendezvous, die. exec'd rather
   than forked so the attach's death-listener thread starts in a fresh
   process image (TSan forbids a thread start after a multi-threaded
   fork, and the test process has long armed its own listener). _exit
   skips the atexit leak report, as below. */
static void gone_peer(const char *token) {
  mizu_binding b;
  mizu_binding_bytes(&b);
  mizu_channel *peer = NULL;
  if (mizu_channel_attach(&peer, token, &b) != MIZU_OK) _exit(2);
  if (mizu_channel_ready_set(peer) != MIZU_OK) _exit(3);
  _exit(0);                        /* dies holding no close bit */
}

/* The held-lock peer child (exec'd): attach, take the lock, then fork a
   squatter grandchild that re-takes the lock on its own plain fd once
   our death releases it (the atfork registry closes its inherited
   library fd — a squatter holds by no library fd at all), signals, and
   lingers ~2 s. The host's probe then reads HELD past the peer's death
   until the squatter exits. _exit skips the atexit leak report (ASan's
   LSan would flag the intentionally leaked handle and mask the exit
   code). */
static void held_lock_peer(const char *token, const char *lock_path,
                          int signal_fd) {
  mizu_binding b;
  mizu_binding_bytes(&b);
  mizu_channel *peer = NULL;
  if (mizu_channel_attach(&peer, token, &b) != MIZU_OK) _exit(2);
  if (mizu_channel_ready_set(peer) != MIZU_OK) _exit(3);
  int fd = open(lock_path, O_RDWR);      /* deliberately not the library's */
  if (fd < 0) _exit(4);
  if (fork() == 0) {
    if (flock(fd, LOCK_EX) != 0) _exit(5);   /* unblocks at the peer's death */
    if (write(signal_fd, "1", 1) != 1) _exit(6);
    sleep(2);                                /* holds the lock a while */
    _exit(0);
  }
  _exit(0);                              /* the lock holder dies */
}

/* Cross-thread waits: one end parked in recv, the other publishing. The
   flush's unpark, the spin catch, the budget learn, and the wake-register
   re-checks fire only across a real thread boundary. */
struct chan_thread {
  mizu_channel *c;
  long n;
  mizu_status st;
};

/* RLIMIT_NOFILE caps the fd VALUE, not the count of open fds: with holes
   in the fd space, "exactly k coexisting new fds" means the soft limit
   one past the k-th free value (the k-th free value itself for k = 0).
   Recomputed per level — the failed creates return fds to the holes. */
static rlim_t rlim_for_free(int k) {
  int free_n = 0;
  for (int i = 0; i < 65536; i++) {
    if (fcntl(i, F_GETFD) != -1) continue;
    if (k == 0) return (rlim_t) i;         /* the first free value */
    if (free_n + 1 == k) return (rlim_t) i + 1;   /* the k-th, plus one */
    free_n++;
  }
  return (rlim_t) -1;
}

static void *chan_recv_main(void *arg) {
  struct chan_thread *a = arg;
  a->st = MIZU_OK;
  for (long i = 0; i < a->n; i++) {
    void *obj = NULL;
    mizu_status st = mizu_channel_recv(a->c, &obj, 30000);
    if (st != MIZU_OK) {
      a->st = st;
      break;
    }
    mizu_bytes_free(obj);
  }
  return NULL;
}

static void chan_send_n(mizu_channel *c, long n) {
  mizu_bytes b = { "t", 1 };
  for (long i = 0; i < n; i++) {
    mizu_status st;
    while ((st = mizu_channel_send(c, &b)) == MIZU_FULL)
      ;
    assert(st == MIZU_OK);
  }
}

/* A producer pacing at the spin budget: publishes land in the consumer's
   spin, park-announce, and parked phases alike. A full ring simply
   delays the pace. */
static void *chan_paced_send_main(void *arg) {
  struct chan_thread *a = arg;
  a->st = MIZU_OK;
  mizu_bytes b = { "t", 1 };
  for (long i = 0; i < a->n; i++) {
    double t0 = mizu_now();
    while (mizu_now() - t0 < 16e-6)
      ;
    mizu_status st;
    while ((st = mizu_channel_send(a->c, &b)) == MIZU_FULL)
      ;
    if (st != MIZU_OK) {
      a->st = st;
      break;
    }
  }
  return NULL;
}
#endif

int main(int argc, char **argv) {
  (void) argc; (void) argv;              /* MinGW warns unused on _WIN32 */
#ifndef _WIN32
  if (argc == 3 && strcmp(argv[1], "gonepeer") == 0)
    gone_peer(argv[2]);                  /* never returns */
  if (argc == 5 && strcmp(argv[1], "peer") == 0)
    held_lock_peer(argv[2], argv[3], atoi(argv[4]));   /* never returns */
#endif
  mizu_binding_bytes(&bytesb);

  mizu_channel_opts opts;
  mizu_channel_opts_init(&opts);
  opts.capacity = 8;                 /* small ring: the full path is cheap */
  opts.slot_size = 256;              /* inline_max = 240 */
  opts.arena_size = 64 << 10;
  const char drop[] = "Sbootstrap";  /* a MIZU_DROP_SOURCE-tagged payload */
  opts.drop = (const uint8_t *) drop;
  opts.drop_size = sizeof(drop);

  mizu_channel *host = NULL, *peer = NULL;
  assert(mizu_channel_create(&host, &opts, &bytesb) == MIZU_OK);
  char token[64];
  assert(mizu_channel_token(host, token, sizeof(token)) == MIZU_OK);
  assert(mizu_channel_attach(&peer, token, &bytesb) == MIZU_OK);

  /* the drop is staged at create and readable on the peer */
  const uint8_t *db;
  uint64_t dn;
  mizu_channel_drop(peer, &db, &dn);
  assert(dn == sizeof(drop) && memcmp(db, drop, sizeof(drop)) == 0);

  /* a host without a peer never rendezvous: ready_wait times out */
  mizu_channel *lonely = NULL;
  assert(mizu_channel_create(&lonely, &opts, &bytesb) == MIZU_OK);
  assert(mizu_channel_ready_wait(lonely, 20) == MIZU_TIMEOUT);
  mizu_channel_destroy(lonely);

  /* create validation: the binding check, then every opts gate, refuses
     with the TLS slot set */
  {
    mizu_channel *bad = NULL;
    mizu_binding nb;
    mizu_binding_init(&nb);            /* no stage/read */
    assert(mizu_channel_create(&bad, &opts, &nb) == MIZU_ERR);
    assert(bad == NULL);
    mizu_channel_opts o2 = opts;
    o2.capacity = 3;
    assert(mizu_channel_create(&bad, &o2, &bytesb) == MIZU_ERR);
    o2.capacity = 1;
    assert(mizu_channel_create(&bad, &o2, &bytesb) == MIZU_ERR);
    o2.capacity = 8;
    o2.slot_size = 32;
    assert(mizu_channel_create(&bad, &o2, &bytesb) == MIZU_ERR);
    o2.slot_size = 96;
    assert(mizu_channel_create(&bad, &o2, &bytesb) == MIZU_ERR);
    o2.slot_size = 256;
    o2.arena_size = 32;
    assert(mizu_channel_create(&bad, &o2, &bytesb) == MIZU_ERR);
    o2.arena_size = opts.arena_size;
    o2.drop = NULL;                    /* drop_size without its bytes */
    assert(mizu_channel_create(&bad, &o2, &bytesb) == MIZU_ERR);
    /* a liveness directory path past the wire budget refuses */
#ifndef _WIN32
    char longdir[1000];
    memset(longdir, 'd', sizeof(longdir) - 1);
    longdir[sizeof(longdir) - 1] = '\0';
    assert(setenv("MIZU_LIVENESS_DIR", longdir, 1) == 0);
    assert(mizu_channel_create(&bad, &opts, &bytesb) == MIZU_ERR);
    assert(strstr(mizu_last_error_message(),
                  "liveness directory path too long") != NULL);
    /* a liveness directory that cannot be opened fails at the host
       lock, after the region already exists (the walk-back unlinks) */
    assert(setenv("MIZU_LIVENESS_DIR", "/mizu_no_such_dir_9f8b7c", 1) == 0);
    assert(mizu_channel_create(&bad, &opts, &bytesb) == MIZU_ERR);
    assert(strstr(mizu_last_error_message(),
                  "cannot lock host liveness") != NULL);
    unsetenv("MIZU_LIVENESS_DIR");
#endif
    assert(bad == NULL);
  }

  /* pre-attach the peer's lock is untaken by construction, so no probe
     may read death from it: the full-ring disambiguation returns FULL,
     alive() reports no verdict, and the channel still rendezvouses */
  {
    mizu_channel *ph = NULL, *pp = NULL;
    assert(mizu_channel_create(&ph, &opts, &bytesb) == MIZU_OK);
    assert(mizu_channel_token(ph, token, sizeof(token)) == MIZU_OK);
    for (int i = 0; i < 8; i++) send_bytes(ph, "m", 1);
    mizu_bytes b = { "m", 1 };
    assert(mizu_channel_send(ph, &b) == MIZU_FULL);
    assert(mizu_channel_alive(ph) == 1);
    assert(mizu_channel_attach(&pp, token, &bytesb) == MIZU_OK);
    assert(mizu_channel_ready_set(pp) == MIZU_OK);
    assert(mizu_channel_ready_wait(ph, 5000) == MIZU_OK);
    for (int i = 0; i < 8; i++) recv_bytes(pp, "m", 1);
    send_bytes(ph, "x", 1);
    recv_bytes(pp, "x", 1);
    mizu_channel_destroy(ph);
    mizu_channel_destroy(pp);
  }

  assert(mizu_channel_ready_set(peer) == MIZU_OK);
  assert(mizu_channel_ready_wait(host, 5000) == MIZU_OK);
  /* a repeat ready_wait keeps the armed death watch (no re-arm) */
  assert(mizu_channel_ready_wait(host, 5000) == MIZU_OK);
  assert(mizu_channel_alive(host) == 1);
  assert(mizu_channel_alive(peer) == 1);

  /* attach validation: a region whose preamble fails validation (the
     open half of chan_failed), a token whose region is gone, and a
     token buffer too small */
  {
    mizu_shm *garbage = NULL;
    assert(mizu_shm_create(&garbage, 4096) == MIZU_OK);   /* zero-filled */
    char gtoken[64];
    snprintf(gtoken, sizeof(gtoken), "%s",
             mizu_shm_name(garbage) + strlen(MIZU_PREFIX_LITERAL));
    mizu_channel *gb = NULL;
    assert(mizu_channel_attach(&gb, gtoken, &bytesb) == MIZU_ERR);
    assert(strstr(mizu_last_error_message(), "invalid channel region") !=
           NULL);
    assert(gb == NULL);
    mizu_shm_close(garbage, 1);

    mizu_channel *dup = NULL;
    assert(mizu_channel_attach(&dup, token, &bytesb) == MIZU_ERR);
    assert(dup == NULL);

    /* the token grammar: the charset gate, then the length budget */
    assert(mizu_channel_attach(&dup, "nonsense!", &bytesb) == MIZU_ERR);
    assert(strstr(mizu_last_error_message(), "malformed region-name") !=
           NULL);
    char longtok[40];
    memset(longtok, 'a', sizeof(longtok) - 1);
    longtok[sizeof(longtok) - 1] = '\0';
    assert(mizu_channel_attach(&dup, longtok, &bytesb) == MIZU_ERR);
    assert(strstr(mizu_last_error_message(), "malformed region-name") !=
           NULL);
    /* a second peer on an established channel: the peer's liveness lock
       refuses (the token still names the live host channel) */
    assert(mizu_channel_token(host, token, sizeof(token)) == MIZU_OK);
    assert(mizu_channel_attach(&dup, token, &bytesb) == MIZU_ERR);
    assert(strstr(mizu_last_error_message(),
                  "cannot lock peer liveness file") != NULL);
    assert(dup == NULL);

    char tiny[2];
    assert(mizu_channel_token(host, tiny, sizeof(tiny)) == MIZU_ERR);
  }

  /* INLINE round trip */
  send_bytes(host, "hello", 5);
  recv_bytes(peer, "hello", 5);

  /* the keeperless claim costs nothing: immediate kinds grow no
     tx-keeper count */
  assert(mizu_handle_keep_out((const mizu_handle *) host) == 0);

  /* NIL round trip (the empty buffer) */
  send_bytes(host, NULL, 0);
  recv_bytes(peer, NULL, 0);

  /* ARENA round trip: past the 240-byte inline budget, within the arena.
     The chunk is the one retain — no keeper kind, but its bytes hold
     until the consumer drains (aalloc ahead of afree) */
  unsigned char mid[4096];
  for (size_t i = 0; i < sizeof(mid); i++) mid[i] = (unsigned char) i;
  send_bytes(host, mid, sizeof(mid));
  assert(mizu_handle_keep_out((const mizu_handle *) host) == 0);
  recv_bytes(peer, mid, sizeof(mid));

  /* SHM_RAW: one tx keeper, reaped on the verb after consumption */
  assert(mizu_handle_keep_out((const mizu_handle *) host) == 0);

  /* SHM_RAW round trip: past the arena, a spill region; then the
     free-list recycle + open-cache hit on the second pass */
  unsigned char *big = malloc(1 << 20);
  assert(big != NULL);
  for (size_t i = 0; i < (size_t) (1 << 20); i++)
    big[i] = (unsigned char) (i * 31);
  send_bytes(host, big, 1 << 20);
  assert(mizu_handle_keep_out((const mizu_handle *) host) == 1);
  recv_bytes(peer, big, 1 << 20);

  mizu_channel_info info;
  assert(mizu_channel_info_get(host, &info) == MIZU_OK);
  assert(info.fl_entries == 0);      /* pinned by the outstanding keeper */
  send_bytes(host, "x", 1);          /* the reap trigger: head advanced */
  assert(mizu_handle_keep_out((const mizu_handle *) host) == 0);
  recv_bytes(peer, "x", 1);
  assert(mizu_channel_info_get(host, &info) == MIZU_OK);
  assert(info.fl_entries == 1);      /* surrendered to the free list */

  send_bytes(host, big, 1 << 20);    /* pops the free list (same region) */
  recv_bytes(peer, big, 1 << 20);
  free(big);
  assert(mizu_channel_info_get(host, &info) == MIZU_OK);
  assert(info.fl_hits == 1);
  assert(mizu_channel_info_get(peer, &info) == MIZU_OK);
  assert(info.open_hits == 1);       /* the recycled name hit the cache */

  /* batch: one flush, order preserved */
  {
    mizu_bytes parts[4] = {
      { "a", 1 }, { "bb", 2 }, { "ccc", 3 }, { "dddd", 4 }
    };
    void *objs[4] = { parts, parts + 1, parts + 2, parts + 3 };
    size_t accepted = 0;
    assert(mizu_channel_send_batch(host, objs, 4, &accepted) == MIZU_OK);
    assert(accepted == 4);
    void *got[4];
    size_t n = 0;
    assert(mizu_channel_recv_batch(peer, got, 4, &n, 1000) == MIZU_OK);
    assert(n == 4);
    for (size_t i = 0; i < 4; i++) {
      mizu_bytes *b = got[i];
      assert(b->len == i + 1 &&
             memcmp(b->data, parts[i].data, b->len) == 0);
      mizu_bytes_free(b);
    }
  }

  /* batch, sink form: same drain, each message delivered as read */
  {
    mizu_bytes parts[2] = { { "e", 1 }, { "ff", 2 } };
    void *objs[2] = { parts, parts + 1 };
    size_t accepted = 0;
    assert(mizu_channel_send_batch(host, objs, 2, &accepted) == MIZU_OK);
    assert(accepted == 2);
    void *got[2] = { NULL, NULL };
    size_t n = 0;
    assert(mizu_channel_recv_batch_fn(peer, 4, &n, array_sink, got,
                                     1000) == MIZU_OK);
    assert(n == 2);
    for (size_t i = 0; i < 2; i++) {
      mizu_bytes *b = got[i];
      assert(b->len == i + 1 &&
             memcmp(b->data, parts[i].data, b->len) == 0);
      mizu_bytes_free(b);
    }
    assert(mizu_channel_recv_batch_fn(peer, 4, &n, NULL, got, 0) ==
           MIZU_ERR);
  }

  /* ring full: cap 8 with nothing drained; the 9th send is refused.
     The consumer publishes its head every 32 messages or on drain-empty,
     so a slot frees for the sender only once the ring drains */
  for (int i = 0; i < 8; i++) send_bytes(host, "m", 1);
  {
    mizu_bytes b = { "m", 1 };
    void *obj = NULL;
    assert(mizu_channel_send(host, &b) == MIZU_FULL);
    for (int i = 0; i < 8; i++) {
      assert(mizu_channel_recv(peer, &obj, 1000) == MIZU_OK);
      mizu_bytes_free(obj);
    }
    assert(mizu_channel_send(host, &b) == MIZU_OK);
    assert(mizu_channel_recv(peer, &obj, 1000) == MIZU_OK);
    mizu_bytes_free(obj);
  }

  /* timeout: an empty poll returns TIMEOUT; a sub-spin-budget deadline
     expires inside the spin, so the park call site itself refuses */
  {
    void *obj = NULL;
    assert(mizu_channel_recv(peer, &obj, 20) == MIZU_TIMEOUT);
    assert(obj == NULL);
    assert(mizu_channel_recv(peer, &obj, 0.001) == MIZU_TIMEOUT);
    assert(obj == NULL);
  }

  /* the bytes reader refuses a view-tier frame, without consuming it */
  {
    mizu_binding vb = bytesb;
    vb.stage = stage_shm_vec;
    mizu_channel *vhost = NULL, *vpeer = NULL;
    opts.drop = NULL;
    opts.drop_size = 0;
    assert(mizu_channel_create(&vhost, &opts, &vb) == MIZU_OK);
    assert(mizu_channel_token(vhost, token, sizeof(token)) == MIZU_OK);
    assert(mizu_channel_attach(&vpeer, token, &bytesb) == MIZU_OK);
    assert(mizu_channel_ready_set(vpeer) == MIZU_OK);
    assert(mizu_channel_ready_wait(vhost, 5000) == MIZU_OK);
    mizu_bytes b = { "v", 1 };
    assert(mizu_channel_send(vhost, &b) == MIZU_OK);
    void *obj = NULL;
    assert(mizu_channel_recv(vpeer, &obj, 1000) == MIZU_ERR);
    assert(mizu_channel_errcat(vpeer) == MIZU_ERRCAT_OTHER);
    assert(mizu_channel_recv(vpeer, &obj, 1000) == MIZU_ERR);  /* wedged */
    mizu_channel_destroy(vpeer);
    mizu_channel_destroy(vhost);

    /* batch form: the failure first errors with the record; past a good
       prefix it ends the batch early with MIZU_OK and stays as the next
       receive's head */
    assert(mizu_channel_create(&vhost, &opts, &vb) == MIZU_OK);
    assert(mizu_channel_token(vhost, token, sizeof(token)) == MIZU_OK);
    assert(mizu_channel_attach(&vpeer, token, &bytesb) == MIZU_OK);
    assert(mizu_channel_ready_set(vpeer) == MIZU_OK);
    assert(mizu_channel_ready_wait(vhost, 5000) == MIZU_OK);
    /* an empty ring on a zero timeout: TIMEOUT, nothing delivered */
    size_t bn = 9;
    void *bgot[2] = { NULL, NULL };
    assert(mizu_channel_recv_batch_fn(vpeer, 4, &bn, array_sink, bgot,
                                     0) == MIZU_TIMEOUT);
    assert(bn == 0);
    assert(mizu_channel_send(vhost, &b) == MIZU_OK);
    assert(mizu_channel_recv_batch_fn(vpeer, 4, &bn, array_sink, bgot,
                                     1000) == MIZU_ERR);
    assert(bn == 0);
    assert(strcmp(mizu_channel_error(vpeer), "payload read failed") == 0);
    mizu_channel_destroy(vpeer);
    mizu_channel_destroy(vhost);

    mizu_binding mb = bytesb;
    mb.stage = stage_modal_vec;
    assert(mizu_channel_create(&vhost, &opts, &mb) == MIZU_OK);
    assert(mizu_channel_token(vhost, token, sizeof(token)) == MIZU_OK);
    assert(mizu_channel_attach(&vpeer, token, &bytesb) == MIZU_OK);
    assert(mizu_channel_ready_set(vpeer) == MIZU_OK);
    assert(mizu_channel_ready_wait(vhost, 5000) == MIZU_OK);
    vec_next = 0;
    assert(mizu_channel_send(vhost, &b) == MIZU_OK);   /* a good frame */
    vec_next = 1;
    assert(mizu_channel_send(vhost, &b) == MIZU_OK);   /* then the view */
    assert(mizu_channel_recv_batch_fn(vpeer, 4, &bn, array_sink, bgot,
                                     1000) == MIZU_OK);
    assert(bn == 1);
    mizu_bytes_free(bgot[0]);
    vec_next = 0;
    assert(mizu_channel_recv(vpeer, &obj, 1000) == MIZU_ERR);  /* deferred */
    mizu_channel_destroy(vpeer);
    mizu_channel_destroy(vhost);
  }

  /* the modal binding: stage failure surfaces as MIZU_ERRCAT_STAGE; an
     abandoned checkout rolls back at the next verb; pins drop at the
     consumer-done reap */
  {
    mizu_binding pb = bytesb;
    pb.stage = wrap_stage;
    pb.drop = count_drop;
    drops = 0;
    mizu_channel *phost = NULL, *ppeer = NULL;
    assert(mizu_channel_create(&phost, &opts, &pb) == MIZU_OK);
    assert(mizu_channel_token(phost, token, sizeof(token)) == MIZU_OK);
    assert(mizu_channel_attach(&ppeer, token, &bytesb) == MIZU_OK);
    assert(mizu_channel_ready_set(ppeer) == MIZU_OK);
    assert(mizu_channel_ready_wait(phost, 5000) == MIZU_OK);

    mizu_bytes b = { "y", 1 };
    stage_mode = 1;
    assert(mizu_channel_send(phost, &b) == MIZU_ERR);
    assert(mizu_channel_errcat(phost) == MIZU_ERRCAT_STAGE);

    stage_mode = 2;                  /* retain, then abandon */
    assert(mizu_channel_send(phost, &b) == MIZU_ERR);
    stage_mode = 0;
    send_bytes(phost, "z", 1);       /* rolls the checkout back */
    assert(mizu_channel_info_get(phost, &info) == MIZU_OK);
    assert(info.fl_entries == 1);
    recv_bytes(ppeer, "z", 1);

    for (int i = 0; i < 4; i++) send_bytes(phost, "p", 1);
    for (int i = 0; i < 4; i++) recv_bytes(ppeer, "p", 1);
    /* one more verb to reap past the last pin */
    void *obj = NULL;
    assert(mizu_channel_recv(phost, &obj, 0) == MIZU_TIMEOUT);
    assert(drops == 5);              /* "z" and the four "p" */
    mizu_channel_destroy(phost);
    mizu_channel_destroy(ppeer);
  }

  /* the interrupt abandon: a nonzero check unwinds the wait as
     MIZU_ERR / MIZU_ERRCAT_INTERRUPTED, consuming nothing. On a
     pure-spin channel the check fires every spin cycle — no park
     latency in the test */
  {
    mizu_binding ib = bytesb;
    ib.check = check_intr;
    mizu_channel_opts spin_opts = opts;
    spin_opts.flags = MIZU_CHANNEL_SPIN;
    mizu_channel *ihost = NULL, *ipeer = NULL;
    assert(mizu_channel_create(&ihost, &spin_opts, &ib) == MIZU_OK);
    assert(mizu_channel_token(ihost, token, sizeof(token)) == MIZU_OK);
    assert(mizu_channel_attach(&ipeer, token, &bytesb) == MIZU_OK);
    assert(mizu_channel_ready_set(ipeer) == MIZU_OK);
    check_calls = 0;                 /* count only the recv wait's polls */
    assert(mizu_channel_ready_wait(ihost, 5000) == MIZU_OK);
    void *obj = NULL;
    assert(mizu_channel_recv(ihost, &obj, 30000) == MIZU_ERR);
    assert(mizu_channel_errcat(ihost) == MIZU_ERRCAT_INTERRUPTED);
    /* the handle is still consistent: traffic flows */
    send_bytes(ipeer, "q", 1);
    check_calls = -1000000;          /* re-arm the hook */
    recv_bytes(ihost, "q", 1);
    mizu_channel_destroy(ihost);
    mizu_channel_destroy(ipeer);
  }

  /* drain-before-close: published messages of a closed peer are
     complete and valid; the ring reports CLOSED once drained */
  send_bytes(peer, "d1", 2);
  send_bytes(peer, "d2", 2);
  send_bytes(peer, "d3", 2);
  assert(mizu_channel_close_signal(peer) == MIZU_OK);
  recv_bytes(host, "d1", 2);
  recv_bytes(host, "d2", 2);
  recv_bytes(host, "d3", 2);
  {
    void *obj = NULL;
    assert(mizu_channel_recv(host, &obj, 1000) == MIZU_CLOSED);
  }

  /* close rendezvous: the peer's bit is already set */
  assert(mizu_channel_close(host, 5000) == MIZU_OK);
  assert(mizu_channel_alive(host) == 0);   /* released handle */
  /* the released handle keeps the sentinel / closed-error guards */
  {
    mizu_bytes b = { "x", 1 };
    void *obj = NULL;
    void *objs1[1] = { &b };
    void *got1[1] = { NULL };
    size_t ac = 9, nc = 9;
    assert(mizu_channel_send(host, &b) == MIZU_CLOSED);
    assert(mizu_channel_recv(host, &obj, 0) == MIZU_CLOSED);
    assert(mizu_channel_send_batch(host, objs1, 1, &ac) == MIZU_CLOSED &&
           ac == 0);
    assert(mizu_channel_recv_batch_fn(host, 1, &nc, array_sink, got1, 0) ==
           MIZU_CLOSED && nc == 0);
    assert(mizu_channel_token(host, token, sizeof(token)) == MIZU_ERR);
    assert(mizu_channel_ready_set(host) == MIZU_ERR);
    assert(mizu_channel_ready_wait(host, 0) == MIZU_ERR);
    assert(mizu_channel_info_get(host, &info) == MIZU_ERR);
    assert(mizu_channel_peer_ident(host) == 0);
    const uint8_t *db2;
    uint64_t dn2 = 1;
    mizu_channel_drop(host, &db2, &dn2);
    assert(db2 == NULL && dn2 == 0);
  }
  assert(mizu_channel_close(host, 0) == MIZU_OK);   /* idempotent */
  mizu_channel_destroy(host);              /* frees */
  mizu_channel_destroy(peer);

  /* the NULL handle answers the same guards */
  {
    mizu_bytes b = { "x", 1 };
    void *obj = NULL;
    void *objs1[1] = { &b };
    void *got1[1] = { NULL };
    size_t ac = 9, nc = 9;
    assert(mizu_channel_send(NULL, &b) == MIZU_CLOSED);
    assert(mizu_channel_recv(NULL, &obj, 0) == MIZU_CLOSED);
    assert(mizu_channel_send_batch(NULL, objs1, 1, &ac) == MIZU_CLOSED &&
           ac == 0);
    assert(mizu_channel_recv_batch_fn(NULL, 1, &nc, array_sink, got1, 0) ==
           MIZU_CLOSED && nc == 0);
    assert(mizu_channel_token(NULL, token, sizeof(token)) == MIZU_ERR);
    assert(mizu_channel_ready_set(NULL) == MIZU_ERR);
    assert(mizu_channel_ready_wait(NULL, 0) == MIZU_ERR);
    assert(mizu_channel_info_get(NULL, &info) == MIZU_ERR);
    assert(mizu_channel_close_signal(NULL) == MIZU_OK);
    assert(mizu_channel_close(NULL, 0) == MIZU_OK);
    assert(mizu_channel_alive(NULL) == 0);
    assert(mizu_channel_peer_ident(NULL) == 0);
    const uint8_t *db2;
    uint64_t dn2 = 1;
    mizu_channel_drop(NULL, &db2, &dn2);
    assert(db2 == NULL && dn2 == 0);
    mizu_channel_destroy(NULL);            /* no-op */
  }

  /* a send past the sender's own close signal answers CLOSED */
  {
    assert(mizu_channel_create(&host, &opts, &bytesb) == MIZU_OK);
    assert(mizu_channel_token(host, token, sizeof(token)) == MIZU_OK);
    assert(mizu_channel_attach(&peer, token, &bytesb) == MIZU_OK);
    assert(mizu_channel_ready_set(peer) == MIZU_OK);
    assert(mizu_channel_ready_wait(host, 5000) == MIZU_OK);
    mizu_bytes b = { "x", 1 };
    assert(mizu_channel_close_signal(host) == MIZU_OK);
    assert(mizu_channel_send(host, &b) == MIZU_CLOSED);
    mizu_channel_destroy(peer);
    mizu_channel_destroy(host);
  }


  /* close timeout leaves the handle usable; a later signal rendezvous */
  assert(mizu_channel_create(&host, &opts, &bytesb) == MIZU_OK);
  assert(mizu_channel_token(host, token, sizeof(token)) == MIZU_OK);
  assert(mizu_channel_attach(&peer, token, &bytesb) == MIZU_OK);
  assert(mizu_channel_ready_set(peer) == MIZU_OK);
  assert(mizu_channel_ready_wait(host, 5000) == MIZU_OK);
  assert(mizu_channel_close(host, 20) == MIZU_TIMEOUT);
  {
    void *obj = NULL;                /* still usable: recv drains, then
                                        reports the signalled close */
    assert(mizu_channel_recv(host, &obj, 0) == MIZU_CLOSED);
  }
  assert(mizu_channel_close_signal(peer) == MIZU_OK);
  assert(mizu_channel_close(host, 5000) == MIZU_OK);
  mizu_channel_destroy(host);
  mizu_channel_destroy(peer);

  /* destroy on a live handle signals close; the peer drains + closes */
  assert(mizu_channel_create(&host, &opts, &bytesb) == MIZU_OK);
  assert(mizu_channel_token(host, token, sizeof(token)) == MIZU_OK);
  assert(mizu_channel_attach(&peer, token, &bytesb) == MIZU_OK);
  assert(mizu_channel_ready_set(peer) == MIZU_OK);
  assert(mizu_channel_ready_wait(host, 5000) == MIZU_OK);
  send_bytes(host, "bye", 3);
  mizu_channel_destroy(host);         /* signals close, never blocks */
  recv_bytes(peer, "bye", 3);
  {
    void *obj = NULL;
    assert(mizu_channel_recv(peer, &obj, 1000) == MIZU_CLOSED);
  }
  assert(mizu_channel_alive(peer) == 0);   /* the lock probe's verdict */
  mizu_channel_destroy(peer);

  /* destroy with a spilled payload in flight: the vanished region fails
     its one read, then the ring drains to CLOSED instead of wedging
     behind the slot */
  {
    mizu_channel_opts sp = opts;
    sp.arena_size = 0;             /* past-inline frames are SHM_RAW */
    sp.drop = NULL;
    sp.drop_size = 0;
    assert(mizu_channel_create(&host, &sp, &bytesb) == MIZU_OK);
    assert(mizu_channel_token(host, token, sizeof(token)) == MIZU_OK);
    assert(mizu_channel_attach(&peer, token, &bytesb) == MIZU_OK);
    assert(mizu_channel_ready_set(peer) == MIZU_OK);
    assert(mizu_channel_ready_wait(host, 5000) == MIZU_OK);
    unsigned char spill[4096];
    for (size_t i = 0; i < sizeof(spill); i++) spill[i] = (unsigned char) i;
    send_bytes(host, spill, sizeof(spill));   /* SHM_RAW, keeper pinned */
    send_bytes(host, "tail", 4);              /* INLINE behind it */
    mizu_channel_destroy(host);               /* close + retain teardown */
    void *obj = NULL;
    assert(mizu_channel_recv(peer, &obj, 1000) == MIZU_ERR);
    assert(strcmp(mizu_channel_error(peer), "payload region vanished") == 0);
    recv_bytes(peer, "tail", 4);              /* the ring advances */
    assert(mizu_channel_recv(peer, &obj, 1000) == MIZU_CLOSED);
    mizu_channel_destroy(peer);
  }

  /* the preamble validator mirrors the create side's slot range: a torn
     slot < 64 would wrap inline_max into a multi-gigabyte payload bound */
  {
    mizu_preamble p;
    memset(&p, 0, sizeof(p));
    p.magic = MIZU_MAGIC;
    p.version = MIZU_ABI_VERSION;
    p.cap = 8;
    p.slot = 256;
    mizu_preamble out;
    size_t region_size = MIZU_FIXED_LAYOUT_SIZE + 2 * 8 * 256;
    assert(mizu_preamble_validate(&p, region_size, &out) == NULL);
    p.slot = 16;
    assert(mizu_preamble_validate(&p, region_size, &out) != NULL);
    p.slot = 1u << 21;
    assert(mizu_preamble_validate(&p, region_size, &out) != NULL);
  }

  /* a consumer-published head beyond the tail is torn: the reap clamps
     to the tail rather than adopting it (a huge head once spun the
     reap's slot loop) */
  assert(mizu_channel_create(&host, &opts, &bytesb) == MIZU_OK);
  assert(mizu_channel_token(host, token, sizeof(token)) == MIZU_OK);
  assert(mizu_channel_attach(&peer, token, &bytesb) == MIZU_OK);
  assert(mizu_channel_ready_set(peer) == MIZU_OK);
  assert(mizu_channel_ready_wait(host, 5000) == MIZU_OK);
  send_bytes(host, "a", 1);
  {
    mizu_shm *shm = NULL;
    char name[80];
    snprintf(name, sizeof(name), MIZU_PREFIX_LITERAL "%s", token);
    assert(mizu_shm_open_rw(&shm, name, 0) == MIZU_OK);
    int64_t bad = INT64_MAX;
    memcpy((char *) mizu_shm_addr(shm) + MIZU_OFF_HP_HEAD, &bad, 8);
    mizu_shm_close(shm, 0);
    /* fill the ring so the send path runs the forced reap */
    for (int i = 0; i < 4; i++) {
      unsigned char x = (unsigned char) i;
      mizu_bytes xb = { &x, 1 };
      mizu_status st = mizu_channel_send(host, &xb);
      assert(st == MIZU_OK || st == MIZU_FULL);
    }
    recv_bytes(peer, "a", 1);
  }
  mizu_channel_destroy(host);
  mizu_channel_destroy(peer);

  /* the arena gives up after one forced reap: a chunk that still
     doesn't fit falls to the spill tier (5 x 12 KiB fill the 64 KiB
     arena with ring capacity to spare, so the 6th send reaches the
     arena-full path, not the ring-full one) */
  {
    assert(mizu_channel_create(&host, &opts, &bytesb) == MIZU_OK);
    assert(mizu_channel_token(host, token, sizeof(token)) == MIZU_OK);
    assert(mizu_channel_attach(&peer, token, &bytesb) == MIZU_OK);
    assert(mizu_channel_ready_set(peer) == MIZU_OK);
    assert(mizu_channel_ready_wait(host, 5000) == MIZU_OK);
    unsigned char chunk[12288];
    for (size_t i = 0; i < sizeof(chunk); i++) chunk[i] = (unsigned char) i;
    for (int i = 0; i < 5; i++) send_bytes(host, chunk, sizeof(chunk));
    assert(mizu_handle_keep_out((const mizu_handle *) host) == 0);
    send_bytes(host, chunk, sizeof(chunk));       /* arena full: SHM_RAW */
    assert(mizu_handle_keep_out((const mizu_handle *) host) == 1);
    for (int i = 0; i < 6; i++) recv_bytes(peer, chunk, sizeof(chunk));
    mizu_channel_destroy(host);
    mizu_channel_destroy(peer);
  }

  /* a torn ARENA slot (its arena offset poked past the end via the
     shared mapping) is a corrupt payload, never an out-of-bounds read */
  {
    assert(mizu_channel_create(&host, &opts, &bytesb) == MIZU_OK);
    assert(mizu_channel_token(host, token, sizeof(token)) == MIZU_OK);
    assert(mizu_channel_attach(&peer, token, &bytesb) == MIZU_OK);
    assert(mizu_channel_ready_set(peer) == MIZU_OK);
    assert(mizu_channel_ready_wait(host, 5000) == MIZU_OK);
    unsigned char mid[4096];
    for (size_t i = 0; i < sizeof(mid); i++) mid[i] = (unsigned char) i;
    send_bytes(host, mid, sizeof(mid));           /* ARENA at slot 0 */
    mizu_shm *shm = NULL;
    char name[80];
    snprintf(name, sizeof(name), MIZU_PREFIX_LITERAL "%s", token);
    assert(mizu_shm_open_rw(&shm, name, 0) == MIZU_OK);
    uint64_t bad_off = 64u << 20;                 /* past the arena */
    memcpy((char *) mizu_shm_addr(shm) + MIZU_FIXED_LAYOUT_SIZE + 8,
           &bad_off, 8);                          /* slot 0's hdr.aux */
    mizu_shm_close(shm, 0);
    void *obj = NULL;
    assert(mizu_channel_recv(peer, &obj, 1000) == MIZU_ERR);
    assert(strcmp(mizu_channel_error(peer), "corrupt payload slot") == 0);
    mizu_channel_destroy(host);
    mizu_channel_destroy(peer);
  }

  /* a hand-staged RAWSPILL resolves against the arena: in bounds it
     reads the bytes there; out of bounds is the same corrupt verdict */
  {
    mizu_binding rb = bytesb;
    rb.stage = stage_rawspill;
    assert(mizu_channel_create(&host, &opts, &rb) == MIZU_OK);
    assert(mizu_channel_token(host, token, sizeof(token)) == MIZU_OK);
    assert(mizu_channel_attach(&peer, token, &bytesb) == MIZU_OK);
    assert(mizu_channel_ready_set(peer) == MIZU_OK);
    assert(mizu_channel_ready_wait(host, 5000) == MIZU_OK);
    mizu_bytes b = { "x", 1 };
    rawspill_mode = 0;
    assert(mizu_channel_send(host, &b) == MIZU_OK);
    void *obj = NULL;
    assert(mizu_channel_recv(peer, &obj, 1000) == MIZU_OK);
    assert(((mizu_bytes *) obj)->len == 8);   /* the arena's zero bytes */
    mizu_bytes_free(obj);
    rawspill_mode = 1;
    assert(mizu_channel_send(host, &b) == MIZU_OK);
    assert(mizu_channel_recv(peer, &obj, 1000) == MIZU_ERR);
    assert(strcmp(mizu_channel_error(peer), "corrupt payload slot") == 0);
    rawspill_mode = 0;
    mizu_channel_destroy(host);
    mizu_channel_destroy(peer);
  }

  /* a preamble whose livedir_size is torn fails the attach's
     validation */
  {
    assert(mizu_channel_create(&host, &opts, &bytesb) == MIZU_OK);
    assert(mizu_channel_token(host, token, sizeof(token)) == MIZU_OK);
    mizu_shm *shm = NULL;
    char name[80];
    snprintf(name, sizeof(name), MIZU_PREFIX_LITERAL "%s", token);
    assert(mizu_shm_open_rw(&shm, name, 0) == MIZU_OK);
    uint64_t zero = 0;
    memcpy((char *) mizu_shm_addr(shm) + 56, &zero, 8);   /* livedir_size */
    mizu_shm_close(shm, 0);
    mizu_channel *tp = NULL;
    assert(mizu_channel_attach(&tp, token, &bytesb) == MIZU_ERR);
    assert(strstr(mizu_last_error_message(),
                  "liveness directory path") != NULL);
    assert(tp == NULL);
    mizu_channel_destroy(host);
  }

#ifndef _WIN32
  /* threaded waits: a parked consumer is woken by the producer's flush;
     sustained traffic exercises the spin catch, the budget learn, and
     the wake-register re-checks that single-threaded drains never
     reach */
  {
    assert(mizu_channel_create(&host, &opts, &bytesb) == MIZU_OK);
    assert(mizu_channel_token(host, token, sizeof(token)) == MIZU_OK);
    assert(mizu_channel_attach(&peer, token, &bytesb) == MIZU_OK);
    assert(mizu_channel_ready_set(peer) == MIZU_OK);
    assert(mizu_channel_ready_wait(host, 5000) == MIZU_OK);
    pthread_t th;
    struct chan_thread ct;
    /* the deterministic park: the consumer sleeps on its parker until
       the flush's unpark lands */
    ct.c = peer;
    ct.n = 1;
    assert(pthread_create(&th, NULL, chan_recv_main, &ct) == 0);
    {
      struct timespec ts = { 0, 100000000 };   /* 100 ms: parked by now */
      nanosleep(&ts, NULL);
    }
    send_bytes(host, "w", 1);
    assert(pthread_join(th, NULL) == 0);
    assert(ct.st == MIZU_OK);
    /* traffic: the consumer parks, spins, catches, and re-checks */
    ct.c = peer;
    ct.n = 100000;
    assert(pthread_create(&th, NULL, chan_recv_main, &ct) == 0);
    chan_send_n(host, ct.n);
    assert(pthread_join(th, NULL) == 0);
    assert(ct.st == MIZU_OK);
    /* paced sends: publishes land in the park-announce window too */
    ct.c = host;
    ct.n = 20000;
    assert(pthread_create(&th, NULL, chan_paced_send_main, &ct) == 0);
    {
      struct chan_thread cr;
      cr.c = peer;
      cr.n = ct.n;
      chan_recv_main(&cr);
      assert(cr.st == MIZU_OK);
    }
    assert(pthread_join(th, NULL) == 0);
    assert(ct.st == MIZU_OK);
    mizu_channel_destroy(host);
    mizu_channel_destroy(peer);
  }

  /* the pure-spin channel: the wait never parks — a send lands inside
     the spin window, an empty spin burns the deadline */
  {
    mizu_channel_opts so = opts;
    so.flags = MIZU_CHANNEL_SPIN;
    assert(mizu_channel_create(&host, &so, &bytesb) == MIZU_OK);
    assert(mizu_channel_token(host, token, sizeof(token)) == MIZU_OK);
    assert(mizu_channel_attach(&peer, token, &bytesb) == MIZU_OK);
    assert(mizu_channel_ready_set(peer) == MIZU_OK);
    assert(mizu_channel_ready_wait(host, 5000) == MIZU_OK);
    pthread_t th;
    struct chan_thread ct;
    ct.c = peer;
    ct.n = 20000;
    assert(pthread_create(&th, NULL, chan_recv_main, &ct) == 0);
    chan_send_n(host, ct.n);
    assert(pthread_join(th, NULL) == 0);
    assert(ct.st == MIZU_OK);
    void *obj = NULL;
    assert(mizu_channel_recv(peer, &obj, 0.001) == MIZU_TIMEOUT);
    assert(obj == NULL);
    mizu_channel_destroy(host);
    mizu_channel_destroy(peer);
  }

  /* fd exhaustion: one free fd fits the region create's transient fd
     but not a second persistent liveness one; zero free fds fails the
     region create itself. The child restores the soft limit before
     exit so its coverage profile flushes. */
  {
    /* a second channel for the child to attach: its host's liveness
       files must exist before the fork */
    assert(mizu_channel_create(&host, &opts, &bytesb) == MIZU_OK);
    char ptok[64];
    assert(mizu_channel_token(host, ptok, sizeof(ptok)) == MIZU_OK);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
      mizu_shm *warm = NULL;        /* open the child's cached log fd */
      if (mizu_shm_create(&warm, 4096) != MIZU_OK) _exit(2);
      mizu_shm_close(warm, 1);
      struct rlimit rl;
      if (getrlimit(RLIMIT_NOFILE, &rl) != 0) _exit(3);
      rlim_t saved = rl.rlim_cur;
      mizu_pool_opts po;
      mizu_pool_opts_init(&po);
      po.max_workers = 2;
      po.max_submitters = 2;
      po.injection_cap = 8;
      po.per_worker_cap = 8;
      po.result_slots = 16;
      po.slot_size = 256;

      rl.rlim_cur = rlim_for_free(1);   /* one free fd at a time */
      if (setrlimit(RLIMIT_NOFILE, &rl) != 0) _exit(4);
      mizu_channel *fc = NULL;
      if (mizu_channel_create(&fc, &opts, &bytesb) != MIZU_ERR ||
          fc != NULL)
        _exit(5);
      if (strstr(mizu_last_error_message(), "peer liveness") == NULL)
        _exit(6);
      mizu_pool *fp = NULL;
      if (mizu_pool_create(&fp, &po, &bytesb) != MIZU_ERR || fp != NULL)
        _exit(7);
      if (strstr(mizu_last_error_message(), "liveness") == NULL) _exit(8);
      /* attach: the peer-side lock fits, the host file's open doesn't */
      mizu_channel *at = NULL;
      if (mizu_channel_attach(&at, ptok, &bytesb) != MIZU_ERR ||
          at != NULL)
        _exit(25);
      if (strstr(mizu_last_error_message(), "host liveness") == NULL)
        _exit(26);

      rl.rlim_cur = rlim_for_free(4);   /* the four slot files fit */
      if (setrlimit(RLIMIT_NOFILE, &rl) != 0) _exit(9);
      if (mizu_pool_create(&fp, &po, &bytesb) != MIZU_ERR || fp != NULL)
        _exit(10);
      if (strstr(mizu_last_error_message(), "owner liveness") == NULL)
        _exit(11);

      rl.rlim_cur = rlim_for_free(0);       /* zero free fds */
      if (setrlimit(RLIMIT_NOFILE, &rl) != 0) _exit(15);
      if (mizu_channel_create(&fc, &opts, &bytesb) != MIZU_ERR ||
          fc != NULL)
        _exit(16);
      if (strstr(mizu_last_error_message(),
                 "cannot create channel region") == NULL)
        _exit(17);
      if (mizu_pool_create(&fp, &po, &bytesb) != MIZU_ERR || fp != NULL)
        _exit(18);
      if (strstr(mizu_last_error_message(),
                 "cannot create pool region") == NULL)
        _exit(19);
      mizu_shm *fs = NULL;
      if (mizu_shm_create(&fs, 4096) != MIZU_ERR || fs != NULL) _exit(20);
      if (strstr(mizu_last_error_message(), "cannot create region") ==
          NULL)
        _exit(21);
      mizu_handle bh;
      memset(&bh, 0, sizeof(bh));
      mizu_shm *sp = NULL;
      if (mizu_stage_spill_get(&bh, 8192, &sp) != MIZU_ERR || sp != NULL)
        _exit(22);
      if (strstr(mizu_last_error_message(),
                 "cannot create payload region") == NULL)
        _exit(23);

      rl.rlim_cur = saved;
      if (setrlimit(RLIMIT_NOFILE, &rl) != 0) _exit(24);
      exit(0);
    }
    int wst;
    assert(waitpid(pid, &wst, 0) == pid);
    assert(WIFEXITED(wst) && WEXITSTATUS(wst) == 0);
    mizu_channel_destroy(host);
  }

  /* an attach that finds the host's lock acquirable learns the host
     died before the channel was established: the child creates a
     channel, hands the token over, and exits with it unestablished */
  {
    int pipefd[2];
    assert(pipe(pipefd) == 0);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
      close(pipefd[0]);
      mizu_channel *dh = NULL;
      if (mizu_channel_create(&dh, &opts, &bytesb) != MIZU_OK) _exit(2);
      char dtok[64];
      if (mizu_channel_token(dh, dtok, sizeof(dtok)) != MIZU_OK) _exit(3);
      size_t want = strlen(dtok) + 1;
      if (write(pipefd[1], dtok, want) != (ssize_t) want) _exit(4);
      exit(0);                       /* dies holding the host lock */
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
    int wst;
    assert(waitpid(pid, &wst, 0) == pid);
    assert(WIFEXITED(wst) && WEXITSTATUS(wst) == 0);
    mizu_channel *dp = NULL;
    assert(mizu_channel_attach(&dp, dtok, &bytesb) == MIZU_ERR);
    assert(strstr(mizu_last_error_message(),
                  "host died before the channel was established") != NULL);
    assert(dp == NULL);
    /* the failed attach's survivor unlink reaped the orphan's names:
       the region and its two liveness files */
    char name[80];
    snprintf(name, sizeof(name), MIZU_PREFIX_LITERAL "%s", dtok);
    mizu_shm *orphan = NULL;
    assert(mizu_shm_open(&orphan, name) == MIZU_ERR);
    const char *dir = mizu_live_dir();
    char lp[1100];
    snprintf(lp, sizeof(lp), "%s/mizu_%s.live.host", dir, dtok);
    assert(remove(lp) != 0);
    snprintf(lp, sizeof(lp), "%s/mizu_%s.live.peer", dir, dtok);
    assert(remove(lp) != 0);
  }

  /* a peer that dies without its close bit: the lock verdict sticks and
     the send side answers PEER_GONE, not just the wait side */
  {
    assert(mizu_channel_create(&host, &opts, &bytesb) == MIZU_OK);
    assert(mizu_channel_token(host, token, sizeof(token)) == MIZU_OK);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
      execl(argv[0], argv[0], "gonepeer", token, (char *) NULL);
      _exit(9);
    }
    assert(mizu_channel_ready_wait(host, 5000) == MIZU_OK);
    int wst;
    assert(waitpid(pid, &wst, 0) == pid);
    assert(mizu_channel_alive(host) == 0);   /* the lock verdict */
    mizu_bytes b = { "x", 1 };
    assert(mizu_channel_send(host, &b) == MIZU_PEER_GONE);
    void *obj = NULL;
    assert(mizu_channel_recv(host, &obj, 0) == MIZU_PEER_GONE);
    mizu_channel_destroy(host);
  }

  /* the forked-child teardown: the fork guard refuses the raising
     verbs, then abandon frees only the child's copies — the parent's
     channel is unharmed */
  {
    assert(mizu_channel_create(&host, &opts, &bytesb) == MIZU_OK);
    assert(mizu_channel_token(host, token, sizeof(token)) == MIZU_OK);
    assert(mizu_channel_attach(&peer, token, &bytesb) == MIZU_OK);
    assert(mizu_channel_ready_set(peer) == MIZU_OK);
    assert(mizu_channel_ready_wait(host, 5000) == MIZU_OK);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
      mizu_bytes b = { "x", 1 };
      void *obj = NULL;
      if (mizu_channel_send(host, &b) != MIZU_ERR ||
          mizu_channel_errcat(host) != MIZU_ERRCAT_OTHER)
        exit(1);
      if (mizu_channel_recv(peer, &obj, 0) != MIZU_ERR) exit(2);
      /* every raising verb answers the fork guard, token included */
      char ftok[64];
      if (mizu_channel_token(host, ftok, sizeof(ftok)) != MIZU_ERR)
        exit(3);
      void *objs1[1] = { &b };
      size_t ac = 9;
      if (mizu_channel_send_batch(host, objs1, 1, &ac) != MIZU_ERR ||
          ac != 0)
        exit(4);
      void *got1[1] = { NULL };
      size_t nc = 9;
      if (mizu_channel_recv_batch_fn(peer, 1, &nc, array_sink, got1, 0) !=
          MIZU_ERR || nc != 0)
        exit(5);
      mizu_channel_abandon(host);
      mizu_channel_abandon(peer);
      exit(0);
    }
    int wst;
    assert(waitpid(pid, &wst, 0) == pid);
    assert(WIFEXITED(wst) && WEXITSTATUS(wst) == 0);
    send_bytes(host, "alive", 5);
    recv_bytes(peer, "alive", 5);
    mizu_channel_destroy(host);
    mizu_channel_destroy(peer);
  }

  /* the death flag with the lock still HELD (a squatter outlives the
     peer) must not veto the park: recv waits out its own deadline, and
     the verdict lands once the last holder exits */
  {
    assert(mizu_channel_create(&host, &opts, &bytesb) == MIZU_OK);
    assert(mizu_channel_token(host, token, sizeof(token)) == MIZU_OK);
    const char *dir = mizu_live_dir();
    assert(dir != NULL);
    char lpath[1100];
    int ln = snprintf(lpath, sizeof(lpath), "%s/mizu_%s.live.peer",
                      dir, token);
    assert(ln > 0 && (size_t) ln < sizeof(lpath));
    int pipefd[2];
    assert(pipe(pipefd) == 0);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
      char wfd[16];
      snprintf(wfd, sizeof(wfd), "%d", pipefd[1]);
      execl(argv[0], argv[0], "peer", token, lpath, wfd, (char *) NULL);
      _exit(9);
    }
    close(pipefd[1]);
    assert(mizu_channel_ready_wait(host, 5000) == MIZU_OK);
    int wst;
    assert(waitpid(pid, &wst, 0) == pid);   /* the peer is dead ... */
    char c;
    assert(read(pipefd[0], &c, 1) == 1);    /* ... and the squatter holds */
    close(pipefd[0]);
    void *obj = NULL;
    double t0 = mizu_now();
    assert(mizu_channel_recv(host, &obj, 300) == MIZU_TIMEOUT);
    assert(mizu_now() - t0 < 1.0);          /* parked, not spinning */
    assert(mizu_channel_recv(host, &obj, 4000) == MIZU_PEER_GONE);
    mizu_channel_destroy(host);
  }
#endif

  puts("test_channel: ok");
  return 0;
}
