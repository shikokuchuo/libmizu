/* Unit tier: mizu_stage_raw — the raw-tier staging reservation, extracted
   from the mizu/pymizu bindings. Pins the policy table against golden
   (hdr, payload) vectors captured from both bindings pre-extraction
   (2026-09-13, macOS; region names there were 18 bytes — name-length-
   dependent expectations derive from the live handle instead). The churn
   rows force the handle flag directly (it raises on Linux only).
   Assert-based; run via `make test`. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

typedef struct raw_obj_s {
  const void *data;
  uint64_t    n;
  int         wire_type;
} raw_obj;

/* The staged frame as the helper left it. */
static uint32_t g_kind, g_len, g_im;
static uint64_t g_aux;
static uint8_t  g_payload[64];
static int      g_fallback;   /* helper returned NULL: NIL is the marker */

static int rec_stage(void *obj, mizu_slot_hdr *hdr, uint8_t *payload,
                     uint32_t inline_max, mizu_handle *h, void *ctx) {
  (void) ctx;
  raw_obj *o = (raw_obj *) obj;
  g_im = inline_max;
  void *dst = mizu_stage_raw(h, o->n, o->wire_type, hdr, payload, inline_max);
  if (dst == NULL) {
    /* the real bindings fall to their serialized tiers; NIL is the
       unambiguous fallback marker here */
    g_fallback = 1;
    hdr->kind = MIZU_KIND_NIL;
    hdr->len = 0;
    hdr->aux = 0;
  } else {
    g_fallback = 0;
    memcpy(dst, o->data, (size_t) o->n);
  }
  g_kind = hdr->kind;
  g_len = hdr->len;
  g_aux = hdr->aux;
  memcpy(g_payload, payload, sizeof g_payload);
  return 0;
}

static void make_binding(mizu_binding *b) {
  mizu_binding_bytes(b);
  b->stage = rec_stage;
}

static mizu_channel *chan_pair(mizu_binding *b, uint32_t slot_size,
                              uint64_t arena_size, mizu_channel **peer) {
  mizu_channel_opts opts;
  mizu_channel_opts_init(&opts);
  opts.capacity = 8;
  opts.slot_size = slot_size;
  opts.arena_size = arena_size;
  mizu_channel *host = NULL;
  assert(mizu_channel_create(&host, &opts, b) == MIZU_OK);
  char token[64];
  assert(mizu_channel_token(host, token, sizeof(token)) == MIZU_OK);
  assert(mizu_channel_attach(peer, token, b) == MIZU_OK);
  assert(mizu_channel_ready_set(*peer) == MIZU_OK);
  assert(mizu_channel_ready_wait(host, 5000) == MIZU_OK);
  return host;
}

/* Open the named spill region read-only and check the staged bytes. */
static void check_region(const char *name, uint32_t name_len,
                         const raw_obj *o, int shm_vec) {
  char buf[MIZU_NAME_MAX];
  memcpy(buf, name, name_len);
  buf[name_len] = '\0';
  mizu_shm *shm;
  assert(mizu_shm_open(&shm, buf) == MIZU_OK);
  if (shm_vec) {
    int type = 0;
    int64_t n_elems = 0;
    assert(mizu_mizh_check(shm->addr, shm->size, &type, &n_elems) == 0);
    assert(type == o->wire_type);
    assert(n_elems == (int64_t) (o->n / mizu_type_elt_size(o->wire_type)));
    /* the producer loan, stored by retain_zc */
    assert(atomic_load(mizu_zc_rc(shm->addr)) == 1);
    assert(memcmp((const uint8_t *) shm->addr + MIZU_HEADER_SIZE, o->data,
                  (size_t) o->n) == 0);
  } else {
    assert(shm->size >= o->n);
    assert(memcmp(shm->addr, o->data, (size_t) o->n) == 0);
  }
  mizu_shm_close(shm, 0);
}

/* One channel row: stage o through a fresh pair and assert the frame. */
static void chan_row(uint32_t slot_size, uint64_t arena, int churn,
                     raw_obj *o, uint32_t kind, int shm_vec) {
  mizu_binding b;
  make_binding(&b);
  mizu_channel *peer = NULL;
  mizu_channel *host = chan_pair(&b, slot_size, arena, &peer);
  if (churn) ((mizu_handle *) host)->fl.churn = 1;
  assert(mizu_channel_send(host, o) == MIZU_OK);
  assert(g_im == slot_size - 16);
  assert(g_kind == kind);
  if (kind == MIZU_KIND_NIL) {
    assert(g_fallback);
  } else if (kind == MIZU_KIND_RAWVEC) {
    assert(!g_fallback && g_len == o->n &&
           g_aux == (uint64_t) o->wire_type);
    assert(memcmp(g_payload, o->data,
                  o->n < sizeof g_payload ? (size_t) o->n
                                          : sizeof g_payload) == 0);
  } else if (kind == MIZU_KIND_RAWSPILL) {
    /* the channel arena framing: aux = type, payload = 8-byte offset */
    assert(!g_fallback && g_len == o->n &&
           g_aux == (uint64_t) o->wire_type);
    uint64_t off;
    memcpy(&off, g_payload, 8);
    assert(off < arena);
  } else {   /* SHM_VEC: name in the payload, aux = type | total << 8 */
    assert(!g_fallback && shm_vec);
    assert(g_len > 0 && g_len < MIZU_NAME_MAX);
    assert((g_aux & 0xff) == (uint64_t) o->wire_type);
    assert((g_aux >> 8) == (uint64_t) MIZU_HEADER_SIZE + o->n);
    check_region((const char *) g_payload, g_len, o, 1);
  }
  mizu_channel_destroy(host);
  mizu_channel_destroy(peer);
}

int main(void) {
  /* 1 MB of pattern bytes (the widest element is 16 B: CPLX) */
  uint8_t *data = malloc(1 << 20);
  assert(data != NULL);
  for (size_t i = 0; i < (size_t) (1 << 20); i++)
    data[i] = (uint8_t) (i * 31 + 7);

  /* Golden rows: captured from mizu + pymizu 2026-09-13. Channel, slot 256
     (inline 240), arena 4 MB: the arena wins below MIZU_ZC_FLOOR_RAW
     (256 KiB) even past the zc floor; SHM_VEC only past it. */
  const struct { uint64_t n; int type; } rows[] = {
    {0, MIZU_TYPE_RAW}, {8, MIZU_TYPE_REAL}, {240, MIZU_TYPE_REAL},
    {248, MIZU_TYPE_REAL}, {1024, MIZU_TYPE_LGL}, {4096, MIZU_TYPE_CPLX},
    {32760, MIZU_TYPE_REAL}, {32768, MIZU_TYPE_REAL},
    {65536, MIZU_TYPE_INT64}, {262144, MIZU_TYPE_REAL},
    {262152, MIZU_TYPE_REAL}, {1048576, MIZU_TYPE_RAW},
  };
  const uint32_t kinds[] = {
    MIZU_KIND_RAWVEC, MIZU_KIND_RAWVEC, MIZU_KIND_RAWVEC,
    MIZU_KIND_RAWSPILL, MIZU_KIND_RAWSPILL, MIZU_KIND_RAWSPILL,
    MIZU_KIND_RAWSPILL, MIZU_KIND_RAWSPILL,
    MIZU_KIND_RAWSPILL, MIZU_KIND_RAWSPILL,
    MIZU_KIND_SHM_VEC, MIZU_KIND_SHM_VEC,
  };
  for (size_t i = 0; i < sizeof rows / sizeof *rows; i++) {
    raw_obj o = { data, rows[i].n, rows[i].type };
    chan_row(256, 4u << 20, 0, &o, kinds[i], kinds[i] == MIZU_KIND_SHM_VEC);
  }

  /* No arena: below the zc gate the helper declines (the binding
     serializes); at/above it, SHM_VEC. */
  raw_obj small = { data, 1024, MIZU_TYPE_REAL };
  chan_row(256, 0, 0, &small, MIZU_KIND_NIL, 0);
  raw_obj mid = { data, 32768, MIZU_TYPE_REAL };
  chan_row(256, 0, 0, &mid, MIZU_KIND_SHM_VEC, 1);
  raw_obj big = { data, 262152, MIZU_TYPE_REAL };
  chan_row(256, 0, 0, &big, MIZU_KIND_SHM_VEC, 1);

  /* 8 KB arena: a 16 KB frame misses below the gate (fallback), a 64 KB
     frame takes SHM_VEC past it. */
  raw_obj a16k = { data, 16384, MIZU_TYPE_REAL };
  chan_row(256, 8192, 0, &a16k, MIZU_KIND_NIL, 0);
  raw_obj a64k = { data, 65536, MIZU_TYPE_REAL };
  chan_row(256, 8192, 0, &a64k, MIZU_KIND_SHM_VEC, 1);

  /* Churn: the arena serves at any size; no SHM_VEC. */
  raw_obj c300k = { data, 300000, MIZU_TYPE_REAL };
  chan_row(256, 4u << 20, 1, &c300k, MIZU_KIND_RAWSPILL, 0);
  raw_obj c64k = { data, 65536, MIZU_TYPE_REAL };
  chan_row(256, 0, 1, &c64k, MIZU_KIND_NIL, 0);

  /* A larger slot: inline budget 4080 moves the RAWVEC edge. */
  raw_obj w1 = { data, 4080, MIZU_TYPE_REAL };
  chan_row(4096, 4u << 20, 0, &w1, MIZU_KIND_RAWVEC, 0);
  raw_obj w2 = { data, 4081, MIZU_TYPE_RAW };
  chan_row(4096, 4u << 20, 0, &w2, MIZU_KIND_RAWSPILL, 0);

  /* Pool rows (controller, slot 256): RAWSPILL is a named region, SHM_VEC
     at the 32K gate, churn forces RAWSPILL. */
  mizu_pool_opts opts;
  mizu_pool_opts_init(&opts);
  opts.max_workers = 1;
  opts.max_submitters = 1;
  opts.injection_cap = 16;
  opts.per_worker_cap = 16;
  opts.result_slots = 16;
  opts.slot_size = 256;

  mizu_binding b;
  make_binding(&b);
  mizu_pool *ctrl = NULL;
  assert(mizu_pool_create(&ctrl, &opts, &b) == MIZU_OK);

  mizu_task task;
  /* probe the entry inline budget (slot - entry header) with a tiny row */
  raw_obj probe = { data, 8, MIZU_TYPE_REAL };
  assert(mizu_pool_submit(ctrl, &probe, &task, 1000) == MIZU_OK);
  assert(g_kind == MIZU_KIND_RAWVEC && g_im > 0 && g_im < 256);
  const uint64_t im = g_im;

  raw_obj pedge = { data, im, MIZU_TYPE_REAL };
  assert(mizu_pool_submit(ctrl, &pedge, &task, 1000) == MIZU_OK);
  assert(g_kind == MIZU_KIND_RAWVEC && g_len == im &&
         g_aux == MIZU_TYPE_REAL);

  raw_obj pover = { data, im + 8, MIZU_TYPE_REAL };
  assert(mizu_pool_submit(ctrl, &pover, &task, 1000) == MIZU_OK);
  assert(g_kind == MIZU_KIND_RAWSPILL && g_len == im + 8);
  assert((g_aux & 0xff) == MIZU_TYPE_REAL);
  assert((g_aux >> 8) > 0 && (g_aux >> 8) < MIZU_NAME_MAX);
  check_region((const char *) g_payload, (uint32_t) (g_aux >> 8), &pover, 0);

  raw_obj p31k = { data, 32760, MIZU_TYPE_REAL };
  assert(mizu_pool_submit(ctrl, &p31k, &task, 1000) == MIZU_OK);
  assert(g_kind == MIZU_KIND_RAWSPILL && g_len == 32760);
  assert((g_aux & 0xff) == MIZU_TYPE_REAL);

  raw_obj p32k = { data, 32768, MIZU_TYPE_REAL };
  assert(mizu_pool_submit(ctrl, &p32k, &task, 1000) == MIZU_OK);
  assert(g_kind == MIZU_KIND_SHM_VEC);
  assert((g_aux & 0xff) == MIZU_TYPE_REAL &&
         (g_aux >> 8) == (uint64_t) MIZU_HEADER_SIZE + 32768);
  check_region((const char *) g_payload, g_len, &p32k, 1);

  /* int64 is a native wire type end to end */
  raw_obj p64 = { data, 65536, MIZU_TYPE_INT64 };
  assert(mizu_pool_submit(ctrl, &p64, &task, 1000) == MIZU_OK);
  assert(g_kind == MIZU_KIND_SHM_VEC && (g_aux & 0xff) == MIZU_TYPE_INT64);
  check_region((const char *) g_payload, g_len, &p64, 1);

  /* churn: the pool's raw spill is a region either way */
  ((mizu_handle *) ctrl)->fl.churn = 1;
  raw_obj pc = { data, 65536, MIZU_TYPE_REAL };
  assert(mizu_pool_submit(ctrl, &pc, &task, 1000) == MIZU_OK);
  assert(g_kind == MIZU_KIND_RAWSPILL && (g_aux & 0xff) == MIZU_TYPE_REAL);
  check_region((const char *) g_payload, (uint32_t) (g_aux >> 8), &pc, 0);

  mizu_pool_destroy(ctrl);
  free(data);
  puts("test_stage_raw: ok");
  return 0;
}
