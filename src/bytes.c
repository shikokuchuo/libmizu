/* The built-in bytes binding (mizu.h): stage/read for an opaque {ptr, len}
   buffer over the INLINE / ARENA / SHM_RAW tiers — the same tiers a
   serialize stream rides, with the same lifetime discipline (nothing is
   pinned: bare bytes can carry no hook-emitted identifier). This is the
   FFI zero-callback path, the reference stager for binding authors, and
   the C test tiers' stager. A bytes handle is a channel peer or a pool
   submitter, never a worker (exec is NULL). */

#include <stdlib.h>
#include "internal.h"

// Stage ---------------------------------------------------------------------------

/* Frame the buffer: NIL when empty, INLINE within the slot budget, one
   arena chunk past it, else a spill region (a reap first, so the checkout
   sees the freshest consumer-done surrenders). The region is retained
   SPILL — it surrenders to the free list at consumer-done. */
static int mizu_bytes_stage(void *obj, mizu_slot_hdr *hdr, uint8_t *payload,
                           uint32_t inline_max, mizu_handle *h, void *ctx) {
  mizu_bytes *b = (mizu_bytes *) obj;
  (void) ctx;
  size_t n = b->len;

  if (n == 0) {
    hdr->kind = MIZU_KIND_NIL;
    hdr->len = 0;
    hdr->aux = 0;
    return 0;
  }
  if (n <= (size_t) inline_max) {
    memcpy(payload, b->data, n);
    hdr->kind = MIZU_KIND_INLINE;
    hdr->len = (uint32_t) n;
    hdr->aux = MIZU_AUX_F_KEEPERLESS;   /* bare bytes pin nothing */
    return 0;
  }
  uint64_t off;
  unsigned char *chunk = mizu_stage_arena_alloc(h, MIZU_ALIGN64(n), &off);
  if (chunk != NULL) {
    memcpy(chunk, b->data, n);
    hdr->kind = MIZU_KIND_ARENA;
    hdr->len = 0;
    hdr->aux = off;
    uint64_t n64 = (uint64_t) n;
    memcpy(payload, &n64, sizeof(n64));
    return 0;
  }
  mizu_stage_reap(h);
  mizu_shm *shm;
  if (mizu_stage_spill_get(h, n, &shm) != MIZU_OK) return 1;
  memcpy(shm->addr, b->data, n);
  hdr->kind = MIZU_KIND_SHM_RAW;
  hdr->len = (uint32_t) shm->name_len;
  hdr->aux = (uint64_t) n;   /* exact length: a recycled region carries slack */
  memcpy(payload, shm->name, shm->name_len);
  mizu_stage_retain(h, shm);
  return 0;
}

// Read ----------------------------------------------------------------------------

/* Resolve a frame to its byte range: the copy tiers of a foreign peer
   (RAWVEC/RAWSPILL/STR1) read as their bare bytes; the view tiers
   (SHM_VEC/REF) are binding-owned by design and fail the read. */
static int bytes_resolve(const mizu_slot_hdr *hdr, const uint8_t *payload,
                         size_t limit, mizu_read_ctx *ctx,
                         const unsigned char **src_out, size_t *n_out) {
  const unsigned char *src = payload;
  size_t n = 0;

  switch (hdr->kind) {
  case MIZU_KIND_NIL:
    break;
  case MIZU_KIND_INLINE:
  case MIZU_KIND_RAWVEC:
  case MIZU_KIND_STR1:
    /* resolved or slot-resident bytes; len bounds-checked against the
       validated capacity (STR1's NA marker reads as empty) */
    if (hdr->len > limit) return -1;
    n = hdr->len;
    break;
  case MIZU_KIND_ARENA:
    /* resolved stream bytes; limit is the arena-validated length */
    n = limit;
    break;
  case MIZU_KIND_RAWSPILL:
    if (ctx->handle->htype == MIZU_HTYPE_POOL) {
      /* pool framing: the region name in the payload, its length and the
         wire type packed in aux (the channel's arena framing of this
         kind is resolved by the transport before the call) */
      uint32_t name_len = (uint32_t) (hdr->aux >> 8);
      mizu_shm *shm = mizu_read_region(ctx, payload, name_len);
      if (shm == NULL) return -1;
      if (hdr->len > shm->size) return -1;
      src = (const unsigned char *) shm->addr;
      n = hdr->len;
      break;
    }
    if (hdr->len > limit) return -1;
    n = hdr->len;
    break;
  case MIZU_KIND_SHM_RAW: {
    mizu_shm *shm = mizu_read_region(ctx, payload, hdr->len);
    if (shm == NULL) return -1;
    /* aux is the exact stream length: a recycled region is larger than
       the stream it carries, and the slack bytes are a previous
       payload's. A length past the region is a torn header, failed like
       the RAWSPILL branch's — never a silent fallback to the slack. */
    if (hdr->aux > (uint64_t) shm->size) return -1;
    n = (size_t) hdr->aux;
    src = (const unsigned char *) shm->addr;
    break;
  }
  default:
    /* SHM_VEC / REF: view tiers — the mapping would need the zc counted
       add and a release hook, which a plain buffer cannot carry */
    return -1;
  }
  *src_out = src;
  *n_out = n;
  return 0;
}

/* Copy the frame's bytes into a fresh mizu_bytes (data rides the same
   allocation) — ring slots are reused and arena chunks FIFO-reclaim, so
   anything short of a copy dangles past consumer-done. A pool collect's
   non-OK outcome builds no error object: NULL, surfaced by the verb's
   mizu_status. An ERR outcome consumes its slot (a NULL read without
   MIZU_READ_CONSUME would leave the slot ERR until mizu_pool_task_release,
   one leaked result slot per failed task) and carries the envelope's
   bytes in the handle's error record, since a plain-bytes consumer has
   no error object to receive them in. */
static void *mizu_bytes_read(const mizu_slot_hdr *hdr, const uint8_t *payload,
                            size_t limit, mizu_read_ctx *ctx) {
  const unsigned char *src = payload;
  size_t n = 0;
  if (ctx->outcome != MIZU_RS_OK) {
    if (ctx->outcome == MIZU_RS_ERR) {
      ctx->flags |= MIZU_READ_CONSUME;
      if (bytes_resolve(hdr, payload, limit, ctx, &src, &n) == 0 && n != 0)
        mizu_err_record(ctx->handle, MIZU_ERRCAT_OTHER, "task failed: %.*s",
                       (int) (n < 200 ? n : 200), (const char *) src);
      else
        mizu_err_record(ctx->handle, MIZU_ERRCAT_OTHER, "task failed");
    }
    return NULL;
  }
  if (bytes_resolve(hdr, payload, limit, ctx, &src, &n) != 0) return NULL;

  mizu_bytes *b = malloc(sizeof(*b) + (n != 0 ? n : 1));
  if (b == NULL) return NULL;
  b->data = n != 0 ? (void *) (b + 1) : NULL;
  b->len = n;
  if (n != 0) memcpy(b->data, src, n);
  return b;
}

void mizu_binding_bytes(mizu_binding *b) {
  mizu_binding_init(b);
  b->stage = mizu_bytes_stage;
  b->read = mizu_bytes_read;
  b->ident = MIZU_IDENT(MIZU_LANG_BYTES, 0);
}

void mizu_bytes_free(mizu_bytes *b) {
  free(b);
}
