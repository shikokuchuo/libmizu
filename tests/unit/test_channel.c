/* Unit tier: the channel transport, exercised in-process as a
   host/peer pair staging through the built-in bytes binding (the
   consumer path), plus deliberately failing bindings for the
   staging-rollback and interrupt-abandon contracts. Assert-based; run
   via `make test`. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int main(void) {
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

  assert(mizu_channel_ready_set(peer) == MIZU_OK);
  assert(mizu_channel_ready_wait(host, 5000) == MIZU_OK);
  assert(mizu_channel_alive(host) == 1);
  assert(mizu_channel_alive(peer) == 1);

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

  /* timeout: an empty poll returns TIMEOUT */
  {
    void *obj = NULL;
    assert(mizu_channel_recv(peer, &obj, 20) == MIZU_TIMEOUT);
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
  assert(mizu_channel_close(host, 0) == MIZU_OK);   /* idempotent */
  mizu_channel_destroy(host);              /* frees */
  mizu_channel_destroy(peer);

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
    snprintf(name, sizeof(name), "/mizu_%s", token);
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

  puts("test_channel: ok");
  return 0;
}
