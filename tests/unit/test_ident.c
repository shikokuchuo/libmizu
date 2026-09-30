/* Unit tier: the identity words — the channel entity-block exchange
   across ready_set / ready_wait, the mandatory nonzero language byte at
   create / attach / join, unknown capability bits and reserved bits
   8-31 read without rejection, and the pool worker identity word's
   first-join CAS with the exact-match rule. In-process pairs stand in
   for two peers. Assert-based; run via `make test`. */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "mizu_ext.h"

/* A word with unknown capability bits and reserved bits 8-31 set:
   read back verbatim, never rejected. */
#define IDENT_FANCY \
  (MIZU_IDENT(MIZU_LANG_BYTES, 0xFFFFFFF0u) | ((uint64_t) 0xABCu << 8))

static mizu_channel *chan_host(mizu_binding *b) {
  mizu_channel_opts opts;
  mizu_channel_opts_init(&opts);
  opts.capacity = 8;
  opts.slot_size = 256;
  opts.arena_size = 0;
  mizu_channel *c = NULL;
  assert(mizu_channel_create(&c, &opts, b) == MIZU_OK);
  return c;
}

static void channel_tests(void) {
  mizu_binding bh, bp;
  mizu_binding_bytes(&bh);
  mizu_binding_bytes(&bp);
  assert(bh.ident == MIZU_IDENT(MIZU_LANG_BYTES, 0));
  bh.ident = IDENT_FANCY;
  bp.ident = MIZU_IDENT(MIZU_LANG_R, MIZU_CAP_MIZS | MIZU_CAP_MIZL);

  /* a zero language byte is rejected at create and attach */
  mizu_binding b0;
  mizu_binding_bytes(&b0);
  b0.ident = 0;
  mizu_channel_opts opts;
  mizu_channel_opts_init(&opts);
  opts.capacity = 8;
  opts.slot_size = 256;
  mizu_channel *c = NULL;
  assert(mizu_channel_create(&c, &opts, &b0) == MIZU_ERR && c == NULL);
  mizu_channel *h0 = chan_host(&bh);
  char tok0[64];
  assert(mizu_channel_token(h0, tok0, sizeof(tok0)) == MIZU_OK);
  assert(mizu_channel_attach(&c, tok0, &b0) == MIZU_ERR && c == NULL);
  mizu_channel_destroy(h0);

  /* the exchange: 0 before attach, then each side reads the other's
     word — the host after ready_set / ready_wait */
  mizu_channel *host = chan_host(&bh);
  assert(mizu_channel_peer_ident(host) == 0);
  char token[64];
  assert(mizu_channel_token(host, token, sizeof(token)) == MIZU_OK);
  mizu_channel *peer = NULL;
  assert(mizu_channel_attach(&peer, token, &bp) == MIZU_OK);
  assert(mizu_channel_peer_ident(peer) == IDENT_FANCY);
  assert(mizu_channel_ready_set(peer) == MIZU_OK);
  assert(mizu_channel_ready_wait(host, 5000) == MIZU_OK);
  assert(mizu_channel_peer_ident(host) ==
         MIZU_IDENT(MIZU_LANG_R, MIZU_CAP_MIZS | MIZU_CAP_MIZL));
  mizu_channel_destroy(peer);
  mizu_channel_destroy(host);
}

static void pool_tests(void) {
  mizu_pool_opts opts;
  mizu_pool_opts_init(&opts);
  opts.max_workers = 3;
  opts.max_submitters = 2;
  opts.injection_cap = 8;
  opts.per_worker_cap = 8;
  opts.result_slots = 8;
  opts.slot_size = 512;

  /* a zero language byte is rejected at create, attach and join */
  mizu_binding b, b0;
  mizu_binding_bytes(&b);
  mizu_binding_bytes(&b0);
  b0.ident = 0;
  mizu_pool *p = NULL;
  assert(mizu_pool_create(&p, &opts, &b0) == MIZU_ERR && p == NULL);
  assert(mizu_pool_create(&p, &opts, &b) == MIZU_OK);
  char token[64];
  assert(mizu_pool_token(p, token, sizeof(token)) == MIZU_OK);
  mizu_pool *x = NULL;
  assert(mizu_pool_attach(&x, token, &b0) == MIZU_ERR && x == NULL);
  assert(mizu_pool_worker_join(&x, token, 0, &b0) == MIZU_ERR && x == NULL);

  /* a never-joined pool reads 0, on the creator and a submitter alike */
  mizu_pool *sub = NULL;
  assert(mizu_pool_attach(&sub, token, &b) == MIZU_OK);
  assert(mizu_pool_worker_ident(p) == 0);
  assert(mizu_pool_worker_ident(sub) == 0);

  /* the first join fixes the word; a handle opened before it sees the
     word afterwards (read through the mapping, never the copy) */
  mizu_binding w1, w2;
  mizu_binding_bytes(&w1);
  mizu_binding_bytes(&w2);
  w1.ident = IDENT_FANCY;
  w2.ident = MIZU_IDENT(MIZU_LANG_BYTES, 0);   /* differs in caps only */
  mizu_pool *wk = NULL;
  assert(mizu_pool_worker_join(&wk, token, 0, &w1) == MIZU_OK);
  assert(mizu_pool_worker_ident(p) == IDENT_FANCY);
  assert(mizu_pool_worker_ident(sub) == IDENT_FANCY);

  /* an equal word joins; a differing word is rejected... */
  mizu_pool *wk2 = NULL, *wk3 = NULL;
  assert(mizu_pool_worker_join(&wk2, token, 1, &w1) == MIZU_OK);
  assert(mizu_pool_worker_join(&wk3, token, 2, &w2) == MIZU_ERR &&
         wk3 == NULL);
  /* ...having touched no slot state: the slot joins on a retry */
  assert(mizu_pool_worker_join(&wk3, token, 2, &w1) == MIZU_OK);
  assert(mizu_pool_worker_ident(p) == IDENT_FANCY);

  assert(mizu_pool_leave(wk) == MIZU_OK);
  assert(mizu_pool_leave(wk2) == MIZU_OK);
  assert(mizu_pool_leave(wk3) == MIZU_OK);
  mizu_pool_destroy(wk);
  mizu_pool_destroy(wk2);
  mizu_pool_destroy(wk3);
  mizu_pool_destroy(sub);
  mizu_pool_destroy(p);
}

int main(void) {
  channel_tests();
  pool_tests();
  printf("test_ident: OK\n");
  return 0;
}
