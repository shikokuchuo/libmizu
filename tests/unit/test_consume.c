/* Unit tier: the REI_READ_CONSUME read contract — a read_fn that fails a
   slot with the flag consumes it exactly as on success while the verb
   still returns REI_ERR (foreign/corrupt payloads must not wedge the
   ring); without the flag a failed read consumes nothing. Also pins the
   exec_fn read ctx (the handle's template: outcome OK, the handle and
   binding ctx round-tripping). In-process harnesses over the bytes
   binding. Assert-based; run via `make test`. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

static rei_binding bytesb;
static int read_fails;      /* outstanding forced read failures */
static int read_consume;    /* set REI_READ_CONSUME on the failures */

static void *consume_read(const rei_slot_hdr *hdr, const uint8_t *payload,
                          size_t limit, rei_read_ctx *ctx) {
  if (read_fails > 0) {
    read_fails--;
    if (read_consume) ctx->flags |= REI_READ_CONSUME;
    return NULL;
  }
  return bytesb.read(hdr, payload, limit, ctx);
}

static void make_binding(rei_binding *b, rei_exec_fn exec, void *ctx) {
  rei_binding_bytes(b);
  b->read = consume_read;
  b->exec = exec;
  b->ctx = ctx;
}

static void send_bytes(rei_channel *c, const void *data, size_t len) {
  rei_bytes b = { (void *) data, len };
  assert(rei_channel_send(c, &b) == REI_OK);
}

// Channel ----------------------------------------------------------------------

static void channel_tests(void) {
  rei_channel_opts opts;
  rei_channel_opts_init(&opts);
  opts.capacity = 8;
  opts.slot_size = 256;              /* inline_max = 240 */
  opts.arena_size = 64 << 10;

  rei_binding b;
  make_binding(&b, NULL, NULL);
  rei_channel *host = NULL, *peer = NULL;
  assert(rei_channel_create(&host, &opts, &b) == REI_OK);
  char token[64];
  assert(rei_channel_token(host, token, sizeof(token)) == REI_OK);
  assert(rei_channel_attach(&peer, token, &b) == REI_OK);
  assert(rei_channel_ready_set(peer) == REI_OK);
  assert(rei_channel_ready_wait(host, 5000) == REI_OK);

  /* no flag: a failed read consumes nothing — the same slot is retried */
  send_bytes(host, "a", 1);
  send_bytes(host, "b", 1);
  void *obj = NULL;
  read_fails = 2;
  read_consume = 0;
  assert(rei_channel_recv(peer, &obj, 1000) == REI_ERR);
  assert(rei_channel_errcat(peer) == REI_ERRCAT_OTHER);
  assert(rei_channel_recv(peer, &obj, 1000) == REI_ERR);
  assert(rei_channel_recv(peer, &obj, 1000) == REI_OK);   /* "a" again */
  assert(((rei_bytes *) obj)->len == 1 &&
         memcmp(((rei_bytes *) obj)->data, "a", 1) == 0);
  rei_bytes_free(obj);
  assert(rei_channel_recv(peer, &obj, 1000) == REI_OK);
  rei_bytes_free(obj);
  rei_channel_destroy(host);
  rei_channel_destroy(peer);

  /* the flag: the failed slot is consumed, the verb still errors, and a
     retried read sees the NEXT slot. The first payload rides a spill
     region (past the arena): its producer keeper must release after the
     consumed head publishes. A fresh pair: the error-slot assertion
     below needs a handle with no prior record. */
  assert(rei_channel_create(&host, &opts, &b) == REI_OK);
  assert(rei_channel_token(host, token, sizeof(token)) == REI_OK);
  assert(rei_channel_attach(&peer, token, &b) == REI_OK);
  assert(rei_channel_ready_set(peer) == REI_OK);
  assert(rei_channel_ready_wait(host, 5000) == REI_OK);
  unsigned char *big = malloc(1 << 20);
  assert(big != NULL);
  for (size_t i = 0; i < (size_t) (1 << 20); i++)
    big[i] = (unsigned char) (i * 31);
  send_bytes(host, big, 1 << 20);
  send_bytes(host, "next", 4);
  read_fails = 1;
  read_consume = 1;
  assert(rei_channel_recv(peer, &obj, 1000) == REI_ERR);
  /* the binding carries its own message: no generic record lands on the
     fresh handle (the slot would otherwise still read NONE) */
  assert(rei_channel_errcat(peer) == REI_ERRCAT_NONE);
  assert(rei_channel_recv(peer, &obj, 1000) == REI_OK);
  assert(((rei_bytes *) obj)->len == 4 &&
         memcmp(((rei_bytes *) obj)->data, "next", 4) == 0);
  rei_bytes_free(obj);
  free(big);

  /* the consumed slot's keeper releases once the head publication
     reaches the producer (a quiet verb runs the reap) */
  assert(rei_channel_recv(host, &obj, 0) == REI_TIMEOUT);
  rei_channel_info info;
  assert(rei_channel_info_get(host, &info) == REI_OK);
  assert(info.fl_entries == 1);

  rei_channel_destroy(host);
  rei_channel_destroy(peer);
}

// Pool -------------------------------------------------------------------------

static int exec_ctx_cookie;

/* The exec stub: pins the read-ctx contract (a real ctx on the handle,
   outcome OK), then echoes the task frame. */
static int ctx_exec(const rei_slot_hdr *hdr, const uint8_t *payload,
                    size_t limit, rei_result_sink *sink, int catching,
                    rei_read_ctx *ctx) {
  (void) catching;
  assert(ctx != NULL);
  assert(ctx->outcome == REI_RS_OK);
  assert(ctx->handle == (rei_handle *) sink->p);
  assert(ctx->binding_ctx == &exec_ctx_cookie);
  assert(ctx->died_slot == -1 && ctx->gone == 0);
  if (hdr->kind == REI_KIND_NIL) {
    rei_bytes b = { NULL, 0 };
    return rei_result_publish(sink, &b) >= 0 ? 0 : 1;
  }
  assert(hdr->kind == REI_KIND_INLINE && hdr->len <= limit);
  rei_bytes b = { (void *) payload, hdr->len };
  return rei_result_publish(sink, &b) >= 0 ? 0 : 1;
}

static void pool_tests(void) {
  rei_pool_opts opts;
  rei_pool_opts_init(&opts);
  opts.max_workers = 1;
  opts.max_submitters = 1;
  opts.injection_cap = 8;
  opts.per_worker_cap = 8;
  opts.result_slots = 8;
  opts.slot_size = 256;

  rei_binding cb, wb;
  make_binding(&cb, NULL, NULL);
  make_binding(&wb, ctx_exec, &exec_ctx_cookie);
  rei_pool *ctrl = NULL, *wk = NULL;
  assert(rei_pool_create(&ctrl, &opts, &cb) == REI_OK);
  char token[64];
  assert(rei_pool_token(ctrl, token, sizeof(token)) == REI_OK);
  assert(rei_pool_worker_join(&wk, token, 0, &wb) == REI_OK);

  rei_bytes t1 = { "one", 3 }, t2 = { "two", 3 };
  rei_task h1, h2;
  assert(rei_pool_submit(ctrl, &t1, &h1, 1000) == REI_OK);
  assert(rei_pool_submit(ctrl, &t2, &h2, 1000) == REI_OK);
  assert(rei_pool_step(wk, 1000) == REI_STEP_TASK);   /* t1 (the exec ctx
                                                         asserts run here) */
  assert(rei_pool_step(wk, 1000) == REI_STEP_TASK);   /* t2 */

  /* no flag: the failed collect leaves the slot OK — a retry reads it */
  void *v = NULL;
  read_fails = 1;
  read_consume = 0;
  assert(rei_pool_collect(ctrl, &h1, &v, 1000) == REI_ERR);
  assert(rei_pool_collect(ctrl, &h1, &v, 1000) == REI_OK);
  assert(((rei_bytes *) v)->len == 3);
  rei_bytes_free(v);

  /* the flag: the failed collect still frees the slot */
  read_fails = 1;
  read_consume = 1;
  assert(rei_pool_collect(ctrl, &h2, &v, 1000) == REI_ERR);
  rei_pool_status st;
  assert(rei_pool_status_get(ctrl, &st) == REI_OK);
  uint32_t live = 0;
  for (int i = 1; i < 6; i++) live += st.tasks_by_state[i];
  assert(live == 0);
  /* consumed: the handle is spent (FREE reads as already-collected) */
  assert(rei_pool_collect(ctrl, &h2, &v, 1000) == REI_ERR);

  assert(rei_pool_leave(wk) == REI_OK);
  rei_pool_destroy(wk);
  rei_pool_destroy(ctrl);
}

int main(void) {
  rei_binding_bytes(&bytesb);
  channel_tests();
  pool_tests();
  puts("test_consume: ok");
  return 0;
}
