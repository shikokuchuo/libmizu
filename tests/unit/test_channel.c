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

static void send_bytes(rei_channel *c, const void *data, size_t len) {
  rei_bytes b = { (void *) data, len };
  assert(rei_channel_send(c, &b) == REI_OK);
}

static void recv_bytes(rei_channel *c, const void *expect, size_t len) {
  void *obj = NULL;
  assert(rei_channel_recv(c, &obj, 1000) == REI_OK);
  rei_bytes *b = obj;
  assert(b->len == len);
  assert(len == 0 || memcmp(b->data, expect, len) == 0);
  rei_bytes_free(b);
}

// A modal binding wrapping the bytes stager: pin every send, or fail ----

static rei_binding bytesb;
static int pin_token;
static int drops;
static int stage_mode;               /* 0 = bytes+pin, 1 = fail, 2 = spill+fail */

static void count_drop(void *ctx, void *pin) {
  (void) ctx;
  assert(pin == &pin_token);
  drops++;
}

static int wrap_stage(void *obj, rei_slot_hdr *hdr, uint8_t *payload,
                      uint32_t inline_max, rei_handle *h) {
  if (stage_mode == 1) return 1;
  if (stage_mode == 2) {
    /* abandon after retaining: the checkout must roll back at the
       next verb */
    rei_shm *shm;
    if (rei_stage_spill_get(h, 8192, &shm) != REI_OK) return 1;
    rei_stage_retain(h, shm);
    return 1;
  }
  int rc = bytesb.stage(obj, hdr, payload, inline_max, h);
  if (rc == 0) rei_stage_pin(h, &pin_token);
  return rc;
}

/* A frame the bytes reader must refuse (a view tier). */
static int stage_shm_vec(void *obj, rei_slot_hdr *hdr, uint8_t *payload,
                         uint32_t inline_max, rei_handle *h) {
  (void) obj; (void) h;
  const char *name = "/rei_nonexistent";
  hdr->kind = REI_KIND_SHM_VEC;
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
  rei_binding_bytes(&bytesb);

  rei_channel_opts opts;
  rei_channel_opts_init(&opts);
  opts.capacity = 8;                 /* small ring: the full path is cheap */
  opts.slot_size = 256;              /* inline_max = 240 */
  opts.arena_size = 64 << 10;
  const char drop[] = "Sbootstrap";  /* a REI_DROP_SOURCE-tagged payload */
  opts.drop = (const uint8_t *) drop;
  opts.drop_size = sizeof(drop);

  rei_channel *host = NULL, *peer = NULL;
  assert(rei_channel_create(&host, &opts, &bytesb) == REI_OK);
  char token[64];
  assert(rei_channel_token(host, token, sizeof(token)) == REI_OK);
  assert(rei_channel_attach(&peer, token, &bytesb) == REI_OK);

  /* the drop is staged at create and readable on the peer */
  const uint8_t *db;
  uint64_t dn;
  rei_channel_drop(peer, &db, &dn);
  assert(dn == sizeof(drop) && memcmp(db, drop, sizeof(drop)) == 0);

  /* a host without a peer never rendezvous: ready_wait times out */
  rei_channel *lonely = NULL;
  assert(rei_channel_create(&lonely, &opts, &bytesb) == REI_OK);
  assert(rei_channel_ready_wait(lonely, 20) == REI_TIMEOUT);
  rei_channel_destroy(lonely);

  assert(rei_channel_ready_set(peer) == REI_OK);
  assert(rei_channel_ready_wait(host, 5000) == REI_OK);
  assert(rei_channel_alive(host) == 1);
  assert(rei_channel_alive(peer) == 1);

  /* INLINE round trip */
  send_bytes(host, "hello", 5);
  recv_bytes(peer, "hello", 5);

  /* NIL round trip (the empty buffer) */
  send_bytes(host, NULL, 0);
  recv_bytes(peer, NULL, 0);

  /* ARENA round trip: past the 240-byte inline budget, within the arena */
  unsigned char mid[4096];
  for (size_t i = 0; i < sizeof(mid); i++) mid[i] = (unsigned char) i;
  send_bytes(host, mid, sizeof(mid));
  recv_bytes(peer, mid, sizeof(mid));

  /* SHM_RAW round trip: past the arena, a spill region; then the
     free-list recycle + open-cache hit on the second pass */
  unsigned char *big = malloc(1 << 20);
  assert(big != NULL);
  for (size_t i = 0; i < (size_t) (1 << 20); i++)
    big[i] = (unsigned char) (i * 31);
  send_bytes(host, big, 1 << 20);
  recv_bytes(peer, big, 1 << 20);

  rei_channel_info info;
  assert(rei_channel_info_get(host, &info) == REI_OK);
  assert(info.fl_entries == 0);      /* pinned by the outstanding keeper */
  send_bytes(host, "x", 1);          /* the reap trigger: head advanced */
  recv_bytes(peer, "x", 1);
  assert(rei_channel_info_get(host, &info) == REI_OK);
  assert(info.fl_entries == 1);      /* surrendered to the free list */

  send_bytes(host, big, 1 << 20);    /* pops the free list (same region) */
  recv_bytes(peer, big, 1 << 20);
  free(big);
  assert(rei_channel_info_get(host, &info) == REI_OK);
  assert(info.fl_hits == 1);
  assert(rei_channel_info_get(peer, &info) == REI_OK);
  assert(info.open_hits == 1);       /* the recycled name hit the cache */

  /* batch: one flush, order preserved */
  {
    rei_bytes parts[4] = {
      { "a", 1 }, { "bb", 2 }, { "ccc", 3 }, { "dddd", 4 }
    };
    void *objs[4] = { parts, parts + 1, parts + 2, parts + 3 };
    size_t accepted = 0;
    assert(rei_channel_send_batch(host, objs, 4, &accepted) == REI_OK);
    assert(accepted == 4);
    void *got[4];
    size_t n = 0;
    assert(rei_channel_recv_batch(peer, got, 4, &n, 1000) == REI_OK);
    assert(n == 4);
    for (size_t i = 0; i < 4; i++) {
      rei_bytes *b = got[i];
      assert(b->len == i + 1 &&
             memcmp(b->data, parts[i].data, b->len) == 0);
      rei_bytes_free(b);
    }
  }

  /* ring full: cap 8 with nothing drained; the 9th send is refused.
     The consumer publishes its head every 32 messages or on drain-empty,
     so a slot frees for the sender only once the ring drains */
  for (int i = 0; i < 8; i++) send_bytes(host, "m", 1);
  {
    rei_bytes b = { "m", 1 };
    void *obj = NULL;
    assert(rei_channel_send(host, &b) == REI_FULL);
    for (int i = 0; i < 8; i++) {
      assert(rei_channel_recv(peer, &obj, 1000) == REI_OK);
      rei_bytes_free(obj);
    }
    assert(rei_channel_send(host, &b) == REI_OK);
    assert(rei_channel_recv(peer, &obj, 1000) == REI_OK);
    rei_bytes_free(obj);
  }

  /* timeout: an empty poll returns TIMEOUT */
  {
    void *obj = NULL;
    assert(rei_channel_recv(peer, &obj, 20) == REI_TIMEOUT);
    assert(obj == NULL);
  }

  /* the bytes reader refuses a view-tier frame, without consuming it */
  {
    rei_binding vb = bytesb;
    vb.stage = stage_shm_vec;
    rei_channel *vhost = NULL, *vpeer = NULL;
    opts.drop = NULL;
    opts.drop_size = 0;
    assert(rei_channel_create(&vhost, &opts, &vb) == REI_OK);
    assert(rei_channel_token(vhost, token, sizeof(token)) == REI_OK);
    assert(rei_channel_attach(&vpeer, token, &bytesb) == REI_OK);
    assert(rei_channel_ready_set(vpeer) == REI_OK);
    assert(rei_channel_ready_wait(vhost, 5000) == REI_OK);
    rei_bytes b = { "v", 1 };
    assert(rei_channel_send(vhost, &b) == REI_OK);
    void *obj = NULL;
    assert(rei_channel_recv(vpeer, &obj, 1000) == REI_ERR);
    assert(rei_channel_errcat(vpeer) == REI_ERRCAT_OTHER);
    assert(rei_channel_recv(vpeer, &obj, 1000) == REI_ERR);  /* wedged */
    rei_channel_destroy(vpeer);
    rei_channel_destroy(vhost);
  }

  /* the modal binding: stage failure surfaces as REI_ERRCAT_STAGE; an
     abandoned checkout rolls back at the next verb; pins drop at the
     consumer-done reap */
  {
    rei_binding pb = bytesb;
    pb.stage = wrap_stage;
    pb.drop = count_drop;
    drops = 0;
    rei_channel *phost = NULL, *ppeer = NULL;
    assert(rei_channel_create(&phost, &opts, &pb) == REI_OK);
    assert(rei_channel_token(phost, token, sizeof(token)) == REI_OK);
    assert(rei_channel_attach(&ppeer, token, &bytesb) == REI_OK);
    assert(rei_channel_ready_set(ppeer) == REI_OK);
    assert(rei_channel_ready_wait(phost, 5000) == REI_OK);

    rei_bytes b = { "y", 1 };
    stage_mode = 1;
    assert(rei_channel_send(phost, &b) == REI_ERR);
    assert(rei_channel_errcat(phost) == REI_ERRCAT_STAGE);

    stage_mode = 2;                  /* retain, then abandon */
    assert(rei_channel_send(phost, &b) == REI_ERR);
    stage_mode = 0;
    send_bytes(phost, "z", 1);       /* rolls the checkout back */
    assert(rei_channel_info_get(phost, &info) == REI_OK);
    assert(info.fl_entries == 1);
    recv_bytes(ppeer, "z", 1);

    for (int i = 0; i < 4; i++) send_bytes(phost, "p", 1);
    for (int i = 0; i < 4; i++) recv_bytes(ppeer, "p", 1);
    /* one more verb to reap past the last pin */
    void *obj = NULL;
    assert(rei_channel_recv(phost, &obj, 0) == REI_TIMEOUT);
    assert(drops == 5);              /* "z" and the four "p" */
    rei_channel_destroy(phost);
    rei_channel_destroy(ppeer);
  }

  /* the interrupt abandon: a nonzero check unwinds the wait as
     REI_ERR / REI_ERRCAT_INTERRUPTED, consuming nothing. On a
     pure-spin channel the check fires every spin cycle — no park
     latency in the test */
  {
    rei_binding ib = bytesb;
    ib.check = check_intr;
    rei_channel_opts spin_opts = opts;
    spin_opts.flags = REI_CHANNEL_SPIN;
    rei_channel *ihost = NULL, *ipeer = NULL;
    assert(rei_channel_create(&ihost, &spin_opts, &ib) == REI_OK);
    assert(rei_channel_token(ihost, token, sizeof(token)) == REI_OK);
    assert(rei_channel_attach(&ipeer, token, &bytesb) == REI_OK);
    assert(rei_channel_ready_set(ipeer) == REI_OK);
    check_calls = 0;                 /* count only the recv wait's polls */
    assert(rei_channel_ready_wait(ihost, 5000) == REI_OK);
    void *obj = NULL;
    assert(rei_channel_recv(ihost, &obj, 30000) == REI_ERR);
    assert(rei_channel_errcat(ihost) == REI_ERRCAT_INTERRUPTED);
    /* the handle is still consistent: traffic flows */
    send_bytes(ipeer, "q", 1);
    check_calls = -1000000;          /* re-arm the hook */
    recv_bytes(ihost, "q", 1);
    rei_channel_destroy(ihost);
    rei_channel_destroy(ipeer);
  }

  /* drain-before-close: published messages of a closed peer are
     complete and valid; the ring reports CLOSED once drained */
  send_bytes(peer, "d1", 2);
  send_bytes(peer, "d2", 2);
  send_bytes(peer, "d3", 2);
  assert(rei_channel_close_signal(peer) == REI_OK);
  recv_bytes(host, "d1", 2);
  recv_bytes(host, "d2", 2);
  recv_bytes(host, "d3", 2);
  {
    void *obj = NULL;
    assert(rei_channel_recv(host, &obj, 1000) == REI_CLOSED);
  }

  /* close rendezvous: the peer's bit is already set */
  assert(rei_channel_close(host, 5000) == REI_OK);
  assert(rei_channel_alive(host) == 0);   /* released handle */
  assert(rei_channel_close(host, 0) == REI_OK);   /* idempotent */
  rei_channel_destroy(host);              /* frees */
  rei_channel_destroy(peer);

  /* close timeout leaves the handle usable; a later signal rendezvous */
  assert(rei_channel_create(&host, &opts, &bytesb) == REI_OK);
  assert(rei_channel_token(host, token, sizeof(token)) == REI_OK);
  assert(rei_channel_attach(&peer, token, &bytesb) == REI_OK);
  assert(rei_channel_ready_set(peer) == REI_OK);
  assert(rei_channel_ready_wait(host, 5000) == REI_OK);
  assert(rei_channel_close(host, 20) == REI_TIMEOUT);
  {
    void *obj = NULL;                /* still usable: recv drains, then
                                        reports the signalled close */
    assert(rei_channel_recv(host, &obj, 0) == REI_CLOSED);
  }
  assert(rei_channel_close_signal(peer) == REI_OK);
  assert(rei_channel_close(host, 5000) == REI_OK);
  rei_channel_destroy(host);
  rei_channel_destroy(peer);

  /* destroy on a live handle signals close; the peer drains + closes */
  assert(rei_channel_create(&host, &opts, &bytesb) == REI_OK);
  assert(rei_channel_token(host, token, sizeof(token)) == REI_OK);
  assert(rei_channel_attach(&peer, token, &bytesb) == REI_OK);
  assert(rei_channel_ready_set(peer) == REI_OK);
  assert(rei_channel_ready_wait(host, 5000) == REI_OK);
  send_bytes(host, "bye", 3);
  rei_channel_destroy(host);         /* signals close, never blocks */
  recv_bytes(peer, "bye", 3);
  {
    void *obj = NULL;
    assert(rei_channel_recv(peer, &obj, 1000) == REI_CLOSED);
  }
  assert(rei_channel_alive(peer) == 0);   /* the lock probe's verdict */
  rei_channel_destroy(peer);

  puts("test_channel: ok");
  return 0;
}
