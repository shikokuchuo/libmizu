/* Unit tier: the MIZU_READ_CONSUME read contract — a read_fn that fails a
   slot with the flag consumes it exactly as on success while the verb
   still returns MIZU_ERR (foreign/corrupt payloads must not wedge the
   ring); without the flag a failed read consumes nothing. Also pins the
   exec_fn read ctx (the handle's template: outcome OK, the handle and
   binding ctx round-tripping). In-process harnesses over the bytes
   binding. Assert-based; run via `make test`. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

static mizu_binding bytesb;
static int read_fails;      /* outstanding forced read failures */
static int read_consume;    /* set MIZU_READ_CONSUME on the failures */

static void *consume_read(const mizu_slot_hdr *hdr, const uint8_t *payload,
                          size_t limit, mizu_read_ctx *ctx) {
  if (read_fails > 0) {
    read_fails--;
    if (read_consume) ctx->flags |= MIZU_READ_CONSUME;
    return NULL;
  }
  return bytesb.read(hdr, payload, limit, ctx);
}

static void make_binding(mizu_binding *b, mizu_exec_fn exec, void *ctx) {
  mizu_binding_bytes(b);
  b->read = consume_read;
  b->exec = exec;
  b->ctx = ctx;
}

static void send_bytes(mizu_channel *c, const void *data, size_t len) {
  mizu_bytes b = { (void *) data, len };
  assert(mizu_channel_send(c, &b) == MIZU_OK);
}

// Channel ----------------------------------------------------------------------

static void channel_tests(void) {
  mizu_channel_opts opts;
  mizu_channel_opts_init(&opts);
  opts.capacity = 8;
  opts.slot_size = 256;              /* inline_max = 240 */
  opts.arena_size = 64 << 10;

  mizu_binding b;
  make_binding(&b, NULL, NULL);
  mizu_channel *host = NULL, *peer = NULL;
  assert(mizu_channel_create(&host, &opts, &b) == MIZU_OK);
  char token[64];
  assert(mizu_channel_token(host, token, sizeof(token)) == MIZU_OK);
  assert(mizu_channel_attach(&peer, token, &b) == MIZU_OK);
  assert(mizu_channel_ready_set(peer) == MIZU_OK);
  assert(mizu_channel_ready_wait(host, 5000) == MIZU_OK);

  /* no flag: a failed read consumes nothing — the same slot is retried */
  send_bytes(host, "a", 1);
  send_bytes(host, "b", 1);
  void *obj = NULL;
  read_fails = 2;
  read_consume = 0;
  assert(mizu_channel_recv(peer, &obj, 1000) == MIZU_ERR);
  assert(mizu_channel_errcat(peer) == MIZU_ERRCAT_OTHER);
  assert(mizu_channel_recv(peer, &obj, 1000) == MIZU_ERR);
  assert(mizu_channel_recv(peer, &obj, 1000) == MIZU_OK);   /* "a" again */
  assert(((mizu_bytes *) obj)->len == 1 &&
         memcmp(((mizu_bytes *) obj)->data, "a", 1) == 0);
  mizu_bytes_free(obj);
  assert(mizu_channel_recv(peer, &obj, 1000) == MIZU_OK);
  mizu_bytes_free(obj);
  mizu_channel_destroy(host);
  mizu_channel_destroy(peer);

  /* the flag: the failed slot is consumed, the verb still errors, and a
     retried read sees the NEXT slot. The first payload rides a spill
     region (past the arena): its producer keeper must release after the
     consumed head publishes. A fresh pair: the error-slot assertion
     below needs a handle with no prior record. */
  assert(mizu_channel_create(&host, &opts, &b) == MIZU_OK);
  assert(mizu_channel_token(host, token, sizeof(token)) == MIZU_OK);
  assert(mizu_channel_attach(&peer, token, &b) == MIZU_OK);
  assert(mizu_channel_ready_set(peer) == MIZU_OK);
  assert(mizu_channel_ready_wait(host, 5000) == MIZU_OK);
  unsigned char *big = malloc(1 << 20);
  assert(big != NULL);
  for (size_t i = 0; i < (size_t) (1 << 20); i++)
    big[i] = (unsigned char) (i * 31);
  send_bytes(host, big, 1 << 20);
  send_bytes(host, "next", 4);
  read_fails = 1;
  read_consume = 1;
  assert(mizu_channel_recv(peer, &obj, 1000) == MIZU_ERR);
  /* the binding carries its own message: no generic record lands on the
     fresh handle (the slot would otherwise still read NONE) */
  assert(mizu_channel_errcat(peer) == MIZU_ERRCAT_NONE);
  assert(mizu_channel_recv(peer, &obj, 1000) == MIZU_OK);
  assert(((mizu_bytes *) obj)->len == 4 &&
         memcmp(((mizu_bytes *) obj)->data, "next", 4) == 0);
  mizu_bytes_free(obj);
  free(big);

  /* the consumed slot's keeper releases once the head publication
     reaches the producer (a quiet verb runs the reap) */
  assert(mizu_channel_recv(host, &obj, 0) == MIZU_TIMEOUT);
  mizu_channel_info info;
  assert(mizu_channel_info_get(host, &info) == MIZU_OK);
  assert(info.fl_entries == 1);

  mizu_channel_destroy(host);
  mizu_channel_destroy(peer);
}

// Pool -------------------------------------------------------------------------

static int exec_ctx_cookie;

/* The exec stub: pins the read-ctx contract (a real ctx on the handle,
   outcome OK), then echoes the task frame. */
static int ctx_exec(const mizu_slot_hdr *hdr, const uint8_t *payload,
                    size_t limit, mizu_result_sink *sink, int catching,
                    mizu_read_ctx *ctx) {
  (void) catching;
  assert(ctx != NULL);
  assert(ctx->outcome == MIZU_RS_OK);
  assert(ctx->handle == (mizu_handle *) sink->p);
  assert(ctx->binding_ctx == &exec_ctx_cookie);
  assert(ctx->died_slot == -1 && ctx->gone == 0);
  if (hdr->kind == MIZU_KIND_NIL) {
    mizu_bytes b = { NULL, 0 };
    return mizu_result_publish(sink, &b) >= 0 ? 0 : 1;
  }
  assert(hdr->kind == MIZU_KIND_INLINE && hdr->len <= limit);
  mizu_bytes b = { (void *) payload, hdr->len };
  return mizu_result_publish(sink, &b) >= 0 ? 0 : 1;
}

static void pool_tests(void) {
  mizu_pool_opts opts;
  mizu_pool_opts_init(&opts);
  opts.max_workers = 1;
  opts.max_submitters = 1;
  opts.injection_cap = 8;
  opts.per_worker_cap = 8;
  opts.result_slots = 8;
  opts.slot_size = 256;

  mizu_binding cb, wb;
  make_binding(&cb, NULL, NULL);
  make_binding(&wb, ctx_exec, &exec_ctx_cookie);
  mizu_pool *ctrl = NULL, *wk = NULL;
  assert(mizu_pool_create(&ctrl, &opts, &cb) == MIZU_OK);
  char token[64];
  assert(mizu_pool_token(ctrl, token, sizeof(token)) == MIZU_OK);
  assert(mizu_pool_worker_join(&wk, token, 0, &wb) == MIZU_OK);

  mizu_bytes t1 = { "one", 3 }, t2 = { "two", 3 };
  mizu_task h1, h2;
  assert(mizu_pool_submit(ctrl, &t1, &h1, 1000) == MIZU_OK);
  assert(mizu_pool_submit(ctrl, &t2, &h2, 1000) == MIZU_OK);
  assert(mizu_pool_step(wk, 1000) == MIZU_STEP_TASK);   /* t1 (the exec ctx
                                                         asserts run here) */
  assert(mizu_pool_step(wk, 1000) == MIZU_STEP_TASK);   /* t2 */

  /* no flag: the failed collect leaves the slot OK — a retry reads it */
  void *v = NULL;
  read_fails = 1;
  read_consume = 0;
  assert(mizu_pool_collect(ctrl, &h1, &v, 1000) == MIZU_ERR);
  assert(mizu_pool_collect(ctrl, &h1, &v, 1000) == MIZU_OK);
  assert(((mizu_bytes *) v)->len == 3);
  mizu_bytes_free(v);

  /* the flag: the failed collect still frees the slot */
  read_fails = 1;
  read_consume = 1;
  assert(mizu_pool_collect(ctrl, &h2, &v, 1000) == MIZU_ERR);
  mizu_pool_status st;
  assert(mizu_pool_status_get(ctrl, &st) == MIZU_OK);
  uint32_t live = 0;
  for (int i = 1; i < 6; i++) live += st.tasks_by_state[i];
  assert(live == 0);
  /* consumed: the handle is spent (FREE reads as already-collected) */
  assert(mizu_pool_collect(ctrl, &h2, &v, 1000) == MIZU_ERR);

  assert(mizu_pool_leave(wk) == MIZU_OK);
  mizu_pool_destroy(wk);
  mizu_pool_destroy(ctrl);
}

int main(void) {
  mizu_binding_bytes(&bytesb);
  channel_tests();
  pool_tests();
  puts("test_consume: ok");
  return 0;
}
