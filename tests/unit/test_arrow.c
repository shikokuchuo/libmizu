/* Unit tier: the Arrow -> MIZL frame writer (arrow.c). Crafted
   ArrowSchema / ArrowArray structs through mizu_arrow_open / _size /
   _write / _rows, read back through the mizu_miz*_check parsers and
   mizu_ix_open on every blob; one hand-stamped region per layout form
   byte-compared. Assert-based; run via `make test`. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

// Crafting helpers ---------------------------------------------------------------

static void mk_schema(ArrowSchema *s, const char *fmt, const char *name,
                      ArrowSchema **children, int64_t n_children,
                      ArrowSchema *dict, int64_t flags) {
  memset(s, 0, sizeof(*s));
  s->format = fmt;
  s->name = name;
  s->flags = flags;
  s->n_children = n_children;
  s->children = children;
  s->dictionary = dict;
}

static void mk_array(ArrowArray *a, int64_t len, int64_t nulls, int64_t off,
                     int64_t n_buffers, const void **buffers,
                     ArrowArray **children, ArrowArray *dict) {
  memset(a, 0, sizeof(*a));
  a->length = len;
  a->null_count = nulls;
  a->offset = off;
  a->n_buffers = n_buffers;
  a->buffers = buffers;
  a->n_children = -1;
  a->children = children;
  a->dictionary = dict;
}

/* open -> size -> write into a calloc'd region. *a_out takes the handle
   (the caller closes); the returned buffer is the caller's to free. */
static uint8_t *frame_write(const ArrowSchema *schema,
                            const ArrowArray *const *batches, size_t nb,
                            uint32_t caps, size_t *total_out,
                            mizu_arrow_warn *warn, mizu_arrow **a_out) {
  mizu_arrow *a = NULL;
  assert(mizu_arrow_open(&a, schema, batches, nb, caps) == MIZU_OK);
  size_t total = mizu_arrow_size(a);
  uint8_t *buf = calloc(1, total);
  assert(buf != NULL);
  memset(warn, 0, sizeof(*warn));
  mizu_arrow_write(a, buf, warn);
  *total_out = total;
  *a_out = a;
  return buf;
}

/* The parsed frame header. */
static int64_t frame_check(const uint8_t *buf, size_t total, int64_t *aoff,
                           int64_t *asz, int64_t *valid) {
  int64_t n = 0;
  assert(mizu_mizl_check(buf, total, &n, aoff, asz, valid) == 0);
  return n;
}

/* A blob is a well-formed 'I' stream. */
static void blob_check(const uint8_t *blob, size_t len) {
  mizu_ix cur;
  assert(mizu_ix_open(&cur, blob, len) == MIZU_OK);
  mizu_ix_item it;
  for (;;) {
    assert(mizu_ix_next(&cur, &it) == MIZU_OK);
    if (cur.done) break;
  }
  assert(mizu_ix_end(&cur) == MIZU_OK);
}

static void p32(uint8_t *d, size_t off, int32_t v) { memcpy(d + off, &v, 4); }
static void p64(uint8_t *d, size_t off, int64_t v) { memcpy(d + off, &v, 8); }
static void raw(uint8_t *d, size_t off, const void *s, size_t n) {
  memcpy(d + off, s, n);
}

/* memmem, portable. */
static const void *find_span(const void *hay, size_t hay_n, const void *ndl,
                             size_t ndl_n) {
  if (ndl_n == 0 || hay_n < ndl_n) return NULL;
  const uint8_t *h = (const uint8_t *) hay;
  for (size_t i = 0; i + ndl_n <= hay_n; i++)
    if (memcmp(h + i, ndl, ndl_n) == 0) return h + i;
  return NULL;
}

// The hand-stamped regions ----------------------------------------------------------

/* Frame A: "x" int32 {1, -2, NA}, "s" utf8 {"a", NA, "bc"} — 672 bytes. */
static void expect_frame_a(uint8_t *e) {
  p32(e, 0, (int32_t) MIZU_MAGIC_LIST);
  p32(e, 4, 2);
  p64(e, 8, 448);                        /* attrs_off */
  p64(e, 16, 93);                        /* attrs_size */
  p64(e, 40, 640);                       /* validity: table at 640 */
  p64(e, 48, 1);                         /*   one null tallied */
  /* directory: x at 128 (12 bytes INT), s at 192 (195-byte MIZS block) */
  p64(e, 64, 128); p64(e, 72, 12); p32(e, 80, MIZU_TYPE_INT);
  p32(e, 84, 0); p64(e, 88, 3);
  p64(e, 96, 192); p64(e, 104, 195); p32(e, 112, MIZU_TYPE_STR);
  p32(e, 116, 0); p64(e, 120, 3);
  /* the int32 body: 1, the NA sentinel (slot 1 is 0x05's clear bit), 999 */
  p32(e, 128, 1); p32(e, 132, MIZU_NA_INT32); p32(e, 136, 999);
  /* the MIZS block at 192: validity 11111101 (the unused high bits stay
     set — the tail bitmap, producer-verbatim, keeps them clear), offsets
     0/1/1/3, enc 1/0/1 */
  e[192] = 0xFD;
  p64(e, 256, 0); p64(e, 264, 1); p64(e, 272, 1); p64(e, 280, 3);
  e[320] = MIZU_CE_UTF8; e[321] = 0; e[322] = MIZU_CE_UTF8;
  raw(e, 384, "abc", 3);
  /* the root blob at 448 (93 bytes) */
  size_t o = 448;
  e[o] = 0x49; e[o + 1] = 0x01; o += 2;                 /* 'I' header */
  e[o] = 0x0d; p64(e, o + 1, 3); o += 9;                /* dict(3) */
  p32(e, o, 5); raw(e, o + 4, "names", 5); o += 9;      /* key */
  e[o] = 0x0b; p64(e, o + 1, 2); o += 9;                /* strv(2) */
  p32(e, o, 1); e[o + 4] = 'x'; o += 5;
  p32(e, o, 1); e[o + 4] = 's'; o += 5;
  p32(e, o, 5); raw(e, o + 4, "class", 5); o += 9;
  e[o] = 0x04; p32(e, o + 1, 10); raw(e, o + 5, "data.frame", 10); o += 15;
  p32(e, o, 9); raw(e, o + 4, "row.names", 9); o += 13;
  e[o] = 0x07; p64(e, o + 1, 2); o += 9;                /* intv(2) */
  p32(e, o, MIZU_NA_INT32); p32(e, o + 4, -3); o += 8;
  assert(o == 448 + 93);
  /* the tail: x's bitmap at 576, the table at 640 */
  e[576] = 0x05;
  p64(e, 640, 576); p64(e, 648, 1);
  p64(e, 656, 0); p64(e, 664, 0);
}

static void test_frame_a(void) {
  ArrowSchema cx, cs, root;
  ArrowSchema *kids[2] = { &cx, &cs };
  mk_schema(&cx, "i", "x", NULL, 0, NULL, 0);
  mk_schema(&cs, "u", "s", NULL, 0, NULL, 0);
  mk_schema(&root, "+s", NULL, kids, 2, NULL, 0);

  const uint8_t bm_x = 0x05, bm_s = 0x05;
  const int32_t xdata[3] = { 1, -2, 999 };
  const int32_t soffs[4] = { 0, 1, 1, 3 };
  const char sbytes[3] = { 'a', 'b', 'c' };
  const void *xbufs[2] = { &bm_x, xdata };
  const void *sbufs[3] = { &bm_s, soffs, sbytes };
  ArrowArray ax, as, batch;
  ArrowArray *bkids[2] = { &ax, &as };
  mk_array(&ax, 3, 1, 0, 2, xbufs, NULL, NULL);
  mk_array(&as, 3, 1, 0, 3, sbufs, NULL, NULL);
  mk_array(&batch, 3, 0, 0, 1, NULL, bkids, NULL);
  batch.n_children = 2;
  const ArrowArray *batches[1] = { &batch };

  size_t total;
  mizu_arrow_warn warn;
  mizu_arrow *a;
  uint8_t *buf = frame_write(&root, batches, 1, 0, &total, &warn, &a);
  assert(total == 672);
  assert(mizu_arrow_rows(a) == 3);
  assert(warn.n_range == 0 && warn.n_intmin == 0);

  uint8_t *exp = calloc(1, total);
  assert(exp != NULL);
  expect_frame_a(exp);
  assert(memcmp(buf, exp, total) == 0);

  /* the parsers read it back */
  int64_t aoff, asz, valid[2];
  assert(frame_check(buf, total, &aoff, &asz, valid) == 2);
  assert(aoff == 448 && asz == 93 && valid[0] == 640 && valid[1] == 1);
  mizu_mizl_entry e;
  assert(mizu_mizl_elem(buf, total, 0, &e) == 0);
  assert(e.sexptype == MIZU_TYPE_INT && e.length == 3 &&
         e.valid[0] == 576 && e.valid[1] == 1);
  assert(mizu_mizl_elem(buf, total, 1, &e) == 0);
  assert(e.sexptype == MIZU_TYPE_STR && e.valid[0] == 0 &&
         e.valid[1] == 0);
  blob_check(buf + aoff, (size_t) asz);

  free(exp);
  free(buf);
  mizu_arrow_close(a);
}

// The mapped types -------------------------------------------------------------------

static void test_cvt_types(void) {
  /* one column per cvt row, each with a null in slot 1 (uint8 excepted),
     so the tail carries every leaf's bitmap */
  ArrowSchema c[11], root;
  ArrowSchema *kids[11];
  const char *fmts[11] = { "b", "c", "s", "S", "C", "i", "I", "l", "L",
                           "e", "f" };
  const char names[11][2] = { "b", "c", "s", "S", "C", "i", "I", "l", "L",
                              "e", "f" };
  for (int i = 0; i < 11; i++) {
    mk_schema(&c[i], fmts[i], names[i], NULL, 0, NULL, 0);
    kids[i] = &c[i];
  }
  mk_schema(&root, "+s", NULL, kids, 11, NULL, 0);

  const uint8_t bm3 = 0x05;   /* slot 1 null */
  const uint8_t booldata = 0x05;         /* 1, 0, 1 */
  const int8_t i8[3] = { -3, 0, 4 };
  const int16_t i16[3] = { -300, 0, 400 };
  const uint16_t u16[3] = { 60000, 0, 7 };
  const uint8_t u8[3] = { 200, 1, 255 };
  const int32_t i32[3] = { -70000, 0, 8 };
  const uint32_t u32[3] = { 3000000000u, 0, 9 };
  const int64_t i64[3] = { -5000000000LL, 0, 10 };
  const uint64_t u64[3] = { 9007199254740993ULL, 0, 11 };   /* > 2^53 */
  const uint16_t f16[3] = { 0x3C00, 0, 0xC000 };            /* 1.0, -2.0 */
  const float f32[3] = { 1.5f, 0, -2.5f };
  const void *bufs[11][3] = {
    { &bm3, &booldata, NULL }, { &bm3, i8, NULL }, { &bm3, i16, NULL },
    { &bm3, u16, NULL }, { NULL, u8, NULL }, { &bm3, i32, NULL },
    { &bm3, u32, NULL }, { &bm3, i64, NULL }, { &bm3, u64, NULL },
    { &bm3, f16, NULL }, { &bm3, f32, NULL }
  };
  ArrowArray ac[11], batch;
  ArrowArray *bk[11];
  const int64_t nulls[11] = { 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1 };
  for (int i = 0; i < 11; i++) {
    mk_array(&ac[i], 3, nulls[i], 0, 2, bufs[i], NULL, NULL);
    bk[i] = &ac[i];
  }
  mk_array(&batch, 3, 0, 0, 1, NULL, bk, NULL);
  batch.n_children = 11;
  const ArrowArray *batches[1] = { &batch };

  size_t total;
  mizu_arrow_warn warn;
  mizu_arrow *a;
  uint8_t *buf = frame_write(&root, batches, 1, 0, &total, &warn, &a);
  assert(warn.n_range == 1 && warn.n_intmin == 0);
  int64_t aoff, asz, valid[2];
  assert(frame_check(buf, total, &aoff, &asz, valid) == 11);
  assert(valid[1] == 10);   /* every column but uint8 carries one null */

  const int tags[11] = { MIZU_TYPE_LGL, MIZU_TYPE_INT, MIZU_TYPE_INT,
                         MIZU_TYPE_INT, MIZU_TYPE_RAW, MIZU_TYPE_INT,
                         MIZU_TYPE_REAL, MIZU_TYPE_INT64, MIZU_TYPE_REAL,
                         MIZU_TYPE_REAL, MIZU_TYPE_REAL };
  mizu_mizl_entry e;
  for (int i = 0; i < 11; i++) {
    assert(mizu_mizl_elem(buf, total, i, &e) == 0);
    assert(e.sexptype == tags[i] && e.length == 3);
    if (i == 4) assert(e.valid[0] == 0 && e.valid[1] == -1);
    else assert(e.valid[1] == 1);
  }
  /* spot values: bool {1, NA, 1}, u16 widened, u64 past 2^53 -> NA */
  assert(mizu_mizl_elem(buf, total, 0, &e) == 0);
  const int32_t *lgl = (const int32_t *) (buf + e.data_offset);
  assert(lgl[0] == 1 && lgl[1] == MIZU_NA_INT32 && lgl[2] == 1);
  assert(mizu_mizl_elem(buf, total, 3, &e) == 0);
  const int32_t *w16 = (const int32_t *) (buf + e.data_offset);
  assert(w16[0] == 60000 && w16[1] == MIZU_NA_INT32 && w16[2] == 7);
  assert(mizu_mizl_elem(buf, total, 4, &e) == 0);
  assert(memcmp(buf + e.data_offset, u8, 3) == 0);
  assert(mizu_mizl_elem(buf, total, 8, &e) == 0);
  const double *du64 = (const double *) (buf + e.data_offset);
  uint64_t bits;
  memcpy(&bits, &du64[0], 8);
  assert(bits == MIZU_NA_REAL_BITS);   /* past 2^53: the NA payload */
  assert(du64[2] == 11.0);
  assert(mizu_mizl_elem(buf, total, 9, &e) == 0);
  const double *dh = (const double *) (buf + e.data_offset);
  assert(dh[0] == 1.0 && dh[2] == -2.0);
  /* the tail bitmaps: each NA-ful leaf 0x05 */
  for (int i = 0; i < 11; i++) {
    if (i == 4) continue;
    assert(mizu_mizl_elem(buf, total, i, &e) == 0);
    assert(buf[e.valid[0]] == 0x05);
  }
  free(buf);
  mizu_arrow_close(a);
}

static void test_intmin(void) {
  ArrowSchema ci, root;
  ArrowSchema *kids[1] = { &ci };
  mk_schema(&ci, "i", "x", NULL, 0, NULL, 0);
  mk_schema(&root, "+s", NULL, kids, 1, NULL, 0);
  /* slot 0: a genuine INT32_MIN marked valid; slot 1: a real null */
  const uint8_t bm = 0x01;
  const int32_t x[2] = { INT32_MIN, 5 };
  const void *bufs[2] = { &bm, x };
  ArrowArray ax, batch;
  ArrowArray *bk[1] = { &ax };
  mk_array(&ax, 2, 1, 0, 2, bufs, NULL, NULL);
  mk_array(&batch, 2, 0, 0, 1, NULL, bk, NULL);
  batch.n_children = 1;
  const ArrowArray *batches[1] = { &batch };
  size_t total;
  mizu_arrow_warn warn;
  mizu_arrow *a;
  uint8_t *buf = frame_write(&root, batches, 1, 0, &total, &warn, &a);
  assert(warn.n_intmin == 1);   /* the valid one only */
  mizu_mizl_entry e;
  assert(mizu_mizl_elem(buf, total, 0, &e) == 0);
  const int32_t *v = (const int32_t *) (buf + e.data_offset);
  assert(v[0] == INT32_MIN && v[1] == MIZU_NA_INT32);
  free(buf);
  mizu_arrow_close(a);
}

static void test_zero_rows(void) {
  ArrowSchema ci, cs, root;
  ArrowSchema *kids[2] = { &ci, &cs };
  mk_schema(&ci, "i", "x", NULL, 0, NULL, 0);
  mk_schema(&cs, "u", "s", NULL, 0, NULL, 0);
  mk_schema(&root, "+s", NULL, kids, 2, NULL, 0);
  size_t total;
  mizu_arrow_warn warn;
  mizu_arrow *a;
  uint8_t *buf = frame_write(&root, NULL, 0, 0, &total, &warn, &a);
  assert(mizu_arrow_rows(a) == 0);
  int64_t aoff, asz, valid[2];
  assert(frame_check(buf, total, &aoff, &asz, valid) == 2);
  assert(valid[0] == 0 && valid[1] == -1);   /* nothing to tally */
  blob_check(buf + aoff, (size_t) asz);
  mizu_mizl_entry e;
  assert(mizu_mizl_elem(buf, total, 0, &e) == 0 && e.data_size == 0);
  free(buf);
  mizu_arrow_close(a);
}

static void test_chunked_and_offsets(void) {
  /* two batches (2 + 3 rows); the int column sliced to offset 1 in batch
     2, nulls in both; the string column sliced to offset 1 in batch 1 */
  ArrowSchema ci, cs, root;
  ArrowSchema *kids[2] = { &ci, &cs };
  mk_schema(&ci, "i", "x", NULL, 0, NULL, 0);
  mk_schema(&cs, "u", "s", NULL, 0, NULL, 0);
  mk_schema(&root, "+s", NULL, kids, 2, NULL, 0);

  const uint8_t bxm1 = 0x03, bxm2 = 0x0A, bsm1 = 0x04, bsm2 = 0x05;
  const int32_t x1[2] = { 10, 20 };
  const int32_t x2[4] = { -1, 30, 40, 50 };   /* offset 1: 30, 40, 50 */
  const int32_t so1[4] = { 0, 2, 2, 5 };      /* "aa", NA, "bbb" */
  const char sb1[5] = { 'a', 'a', 'b', 'b', 'b' };
  const int32_t so2[4] = { 0, 1, 1, 3 };      /* "c", NA, "dd" */
  const char sb2[3] = { 'c', 'd', 'd' };
  const void *xb1[2] = { &bxm1, x1 };
  const void *xb2[2] = { &bxm2, x2 };
  const void *sb1v[3] = { &bsm1, so1, sb1 };
  const void *sb2v[3] = { &bsm2, so2, sb2 };
  ArrowArray ax1, ax2, as1, as2, b1, b2;
  ArrowArray *k1[2] = { &ax1, &as1 };
  ArrowArray *k2[2] = { &ax2, &as2 };
  mk_array(&ax1, 2, 0, 0, 2, xb1, NULL, NULL);
  mk_array(&ax2, 3, 1, 1, 2, xb2, NULL, NULL);
  mk_array(&as1, 2, 1, 1, 3, sb1v, NULL, NULL);
  mk_array(&as2, 3, 1, 0, 3, sb2v, NULL, NULL);
  mk_array(&b1, 2, 0, 0, 1, NULL, k1, NULL);
  b1.n_children = 2;
  mk_array(&b2, 3, 0, 0, 1, NULL, k2, NULL);
  b2.n_children = 2;
  const ArrowArray *batches[2] = { &b1, &b2 };

  size_t total;
  mizu_arrow_warn warn;
  mizu_arrow *a;
  uint8_t *buf = frame_write(&root, batches, 2, 0, &total, &warn, &a);
  assert(mizu_arrow_rows(a) == 5);
  mizu_mizl_entry e;
  /* x: {10, 20, 30, NA, 50} — the second batch's null is its local slot
     1 (global 3); the tail bitmap rebuilt honouring batch->offset */
  assert(mizu_mizl_elem(buf, total, 0, &e) == 0);
  const int32_t *x = (const int32_t *) (buf + e.data_offset);
  assert(x[0] == 10 && x[1] == 20 && x[2] == 30 && x[3] == MIZU_NA_INT32 &&
         x[4] == 50);
  assert(e.valid[1] == 1 && buf[e.valid[0]] == 0xF7);   /* rebuilt: all
     bits set bar element 3 (the multi-batch arm memsets present first) */
  /* s: {"aa", "c", NA, "dd"} wait — batch 1 sliced to offset 1: rows
     {"c"... } hmm no: batch 1 has 2 rows; the string column's batch 1
     offset 1 takes elements 1..2 of {aa, NA, bbb} = {NA, "bbb"} */
  assert(mizu_mizl_elem(buf, total, 1, &e) == 0);
  const uint8_t *blk = buf + e.data_offset;
  mizu_mizs_geom g = mizu_mizs_geometry(5);
  const int64_t *offs = (const int64_t *) (blk + g.offsets);
  /* {NA, "bbb", "c", NA, "dd"}: offsets 0,0,3,4,4,6 */
  assert(offs[0] == 0 && offs[1] == 0 && offs[2] == 3 && offs[3] == 4 &&
         offs[4] == 4 && offs[5] == 6);
  assert(memcmp(blk + g.data, "bbbcdd", 6) == 0);
  /* validity: nulls at 0 and 3 -> 11110110 = 0xF6 (the MIZS block memsets
     present first, the unused high bits staying set) */
  assert(blk[0] == 0xF6);
  free(buf);
  mizu_arrow_close(a);
}

static void test_null_span_collapse(void) {
  /* a utf8 null whose offsets span is non-empty collapses to zero bytes */
  ArrowSchema cs, root;
  ArrowSchema *kids[1] = { &cs };
  mk_schema(&cs, "u", "s", NULL, 0, NULL, 0);
  mk_schema(&root, "+s", NULL, kids, 1, NULL, 0);
  const uint8_t bm = 0x05;   /* slot 1 null */
  const int32_t so[4] = { 0, 1, 3, 4 };   /* null slot spans [1,3) */
  const char sb[4] = { 'a', 'x', 'x', 'b' };
  const void *sbv[3] = { &bm, so, sb };
  ArrowArray as, batch;
  ArrowArray *bk[1] = { &as };
  mk_array(&as, 3, 1, 0, 3, sbv, NULL, NULL);
  mk_array(&batch, 3, 0, 0, 1, NULL, bk, NULL);
  batch.n_children = 1;
  const ArrowArray *batches[1] = { &batch };
  size_t total;
  mizu_arrow_warn warn;
  mizu_arrow *a;
  uint8_t *buf = frame_write(&root, batches, 1, 0, &total, &warn, &a);
  mizu_mizl_entry e;
  assert(mizu_mizl_elem(buf, total, 0, &e) == 0);
  const uint8_t *blk = buf + e.data_offset;
  mizu_mizs_geom g = mizu_mizs_geometry(3);
  const int64_t *offs = (const int64_t *) (blk + g.offsets);
  assert(offs[0] == 0 && offs[1] == 1 && offs[2] == 1 && offs[3] == 2);
  assert(memcmp(blk + g.data, "ab", 2) == 0);
  free(buf);
  mizu_arrow_close(a);
}

static void test_null_count_forms(void) {
  ArrowSchema ci, root;
  ArrowSchema *kids[1] = { &ci };
  mk_schema(&ci, "i", "x", NULL, 0, NULL, 0);
  mk_schema(&root, "+s", NULL, kids, 1, NULL, 0);
  const int32_t x[3] = { 7, 8, 9 };
  /* null_count = -1, the bitmap carrying the count */
  const uint8_t bm = 0x05;
  const void *bufs[2] = { &bm, x };
  ArrowArray ax, batch;
  ArrowArray *bk[1] = { &ax };
  mk_array(&ax, 3, -1, 0, 2, bufs, NULL, NULL);
  mk_array(&batch, 3, 0, 0, 1, NULL, bk, NULL);
  batch.n_children = 1;
  const ArrowArray *batches[1] = { &batch };
  size_t total;
  mizu_arrow_warn warn;
  mizu_arrow *a;
  uint8_t *buf = frame_write(&root, batches, 1, 0, &total, &warn, &a);
  mizu_mizl_entry e;
  assert(mizu_mizl_elem(buf, total, 0, &e) == 0);
  assert(e.valid[1] == 1 && buf[e.valid[0]] == 0x05);
  const int32_t *v = (const int32_t *) (buf + e.data_offset);
  assert(v[0] == 7 && v[1] == MIZU_NA_INT32 && v[2] == 9);
  free(buf);
  mizu_arrow_close(a);

  /* a bitmap present with zero nulls: the tail collapses to {0, -1} */
  const uint8_t bm0 = 0x07;
  const void *bufs0[2] = { &bm0, x };
  mk_array(&ax, 3, -1, 0, 2, bufs0, NULL, NULL);
  buf = frame_write(&root, batches, 1, 0, &total, &warn, &a);
  int64_t aoff, asz, valid[2];
  assert(frame_check(buf, total, &aoff, &asz, valid) == 1);
  assert(valid[0] == 0 && valid[1] == -1);
  free(buf);
  mizu_arrow_close(a);
}

static void test_nan_vs_na(void) {
  ArrowSchema cg, root;
  ArrowSchema *kids[1] = { &cg };
  mk_schema(&cg, "g", "x", NULL, 0, NULL, 0);
  mk_schema(&root, "+s", NULL, kids, 1, NULL, 0);
  const uint8_t bm = 0x05;   /* slot 1 null */
  double x[3] = { 1.0, 0.0, 0.0 };
  const uint64_t nanbits = 0x7FF0000000000000ULL;   /* a plain NaN: a value */
  memcpy(&x[2], &nanbits, 8);
  const void *bufs[2] = { &bm, x };
  ArrowArray ax, batch;
  ArrowArray *bk[1] = { &ax };
  mk_array(&ax, 3, 1, 0, 2, bufs, NULL, NULL);
  mk_array(&batch, 3, 0, 0, 1, NULL, bk, NULL);
  batch.n_children = 1;
  const ArrowArray *batches[1] = { &batch };
  size_t total;
  mizu_arrow_warn warn;
  mizu_arrow *a;
  uint8_t *buf = frame_write(&root, batches, 1, 0, &total, &warn, &a);
  mizu_mizl_entry e;
  assert(mizu_mizl_elem(buf, total, 0, &e) == 0);
  const double *v = (const double *) (buf + e.data_offset);
  uint64_t b0, b1, b2;
  memcpy(&b0, &v[0], 8);
  memcpy(&b1, &v[1], 8);
  memcpy(&b2, &v[2], 8);
  assert(b0 == 0x3FF0000000000000ULL);
  assert(b1 == MIZU_NA_REAL_BITS);   /* the cvt tier's quiet-bit form */
  assert(b2 == nanbits);             /* NaN crosses as a value */
  assert(buf[e.valid[0]] == 0x05);   /* the NaN's bit stays set */
  free(buf);
  mizu_arrow_close(a);
}

static void test_string_forms(void) {
  /* large_utf8 (i64 offsets) and string_view (16-byte views), chunked */
  ArrowSchema cu, cv, root;
  ArrowSchema *kids[2] = { &cu, &cv };
  mk_schema(&cu, "U", "u", NULL, 0, NULL, 0);
  mk_schema(&cv, "vu", "v", NULL, 0, NULL, 0);
  mk_schema(&root, "+s", NULL, kids, 2, NULL, 0);
  const uint8_t bm = 0x07;   /* no nulls in batch 1; batch 2 below */
  const int64_t uo[3] = { 0, 3, 7 };   /* "foo", "bar!" */
  const char ub[7] = { 'f', 'o', 'o', 'b', 'a', 'r', '!' };
  /* string_view: "hi" inline; a 20-byte string via one data buffer */
  uint8_t views[32];
  memset(views, 0, sizeof views);
  int32_t l0 = 2, l1 = 20;
  memcpy(views, &l0, 4);
  views[4] = 'h';
  views[5] = 'i';
  memcpy(views + 16, &l1, 4);
  char longs[20];
  memset(longs, 'z', sizeof longs);
  int32_t bi = 0, bo = 0;
  memcpy(views + 16 + 8, &bi, 4);
  memcpy(views + 16 + 12, &bo, 4);
  const void *ubufs[3] = { &bm, uo, ub };
  const void *vbufs[3] = { &bm, views, longs };
  ArrowArray au, av, b1;
  ArrowArray *k1[2] = { &au, &av };
  mk_array(&au, 2, 0, 0, 3, ubufs, NULL, NULL);
  mk_array(&av, 2, 0, 0, 3, vbufs, NULL, NULL);
  mk_array(&b1, 2, 0, 0, 1, NULL, k1, NULL);
  b1.n_children = 2;
  const ArrowArray *batches[1] = { &b1 };
  size_t total;
  mizu_arrow_warn warn;
  mizu_arrow *a;
  uint8_t *buf = frame_write(&root, batches, 1, 0, &total, &warn, &a);
  mizu_mizl_entry e;
  assert(mizu_mizl_elem(buf, total, 0, &e) == 0);
  const uint8_t *blk = buf + e.data_offset;
  mizu_mizs_geom g = mizu_mizs_geometry(2);
  const int64_t *offs = (const int64_t *) (blk + g.offsets);
  assert(offs[0] == 0 && offs[1] == 3 && offs[2] == 7);
  assert(memcmp(blk + g.data, "foobar!", 7) == 0);
  assert(mizu_mizl_elem(buf, total, 1, &e) == 0);
  blk = buf + e.data_offset;
  offs = (const int64_t *) (blk + g.offsets);
  assert(offs[0] == 0 && offs[1] == 2 && offs[2] == 22);
  assert(memcmp(blk + g.data, "hi", 2) == 0);
  assert(memcmp(blk + g.data + 2, longs, 20) == 0);
  free(buf);
  mizu_arrow_close(a);
}

// Dictionaries ------------------------------------------------------------------------

/* A dictionary column "d": levels {"low", "high"}, codes {0, 1, 0, NA} as
   int8, chunked 2 + 2; an int32 index width on the second batch. */
static void test_dicts(void) {
  ArrowSchema lv, cd, root;
  ArrowSchema *kids[1] = { &cd };
  mk_schema(&lv, "u", NULL, NULL, 0, NULL, 0);
  mk_schema(&cd, "c", "d", NULL, 0, &lv, 0);
  mk_schema(&root, "+s", NULL, kids, 1, NULL, 0);

  const char levbytes[7] = { 'l', 'o', 'w', 'h', 'i', 'g', 'h' };
  const int32_t levoffs[3] = { 0, 3, 7 };
  ArrowArray dict;
  const void *dbufs[3] = { NULL, levoffs, levbytes };
  mk_array(&dict, 2, 0, 0, 3, dbufs, NULL, NULL);
  const uint8_t bm1 = 0x03, bm2 = 0x01;
  const int8_t codes1[2] = { 0, 1 };
  const int8_t codes2[2] = { 0, 127 };
  const void *cb1[2] = { &bm1, codes1 };
  const void *cb2[2] = { &bm2, codes2 };
  ArrowArray a1, a2, b1, b2;
  ArrowArray *k1[1] = { &a1 };
  ArrowArray *k2[1] = { &a2 };
  mk_array(&a1, 2, 0, 0, 2, cb1, NULL, &dict);
  mk_array(&a2, 2, 1, 0, 2, cb2, NULL, &dict);
  mk_array(&b1, 2, 0, 0, 1, NULL, k1, NULL);
  b1.n_children = 1;
  mk_array(&b2, 2, 0, 0, 1, NULL, k2, NULL);
  b2.n_children = 1;
  const ArrowArray *batches[2] = { &b1, &b2 };
  size_t total;
  mizu_arrow_warn warn;
  mizu_arrow *a;
  uint8_t *buf = frame_write(&root, batches, 2, 0, &total, &warn, &a);
  mizu_mizl_entry e;
  assert(mizu_mizl_elem(buf, total, 0, &e) == 0);
  assert(e.sexptype == MIZU_TYPE_INT && e.attrs_size > 0);
  const int32_t *codes = (const int32_t *) (buf + e.data_offset);
  assert(codes[0] == 1 && codes[1] == 2 && codes[2] == 1 &&
         codes[3] == MIZU_NA_INT32);
  blob_check(buf + e.data_offset + e.data_size - e.attrs_size,
             (size_t) e.attrs_size);
  free(buf);
  mizu_arrow_close(a);
}

static void test_dict_declines(void) {
  ArrowSchema lv, cd, root;
  ArrowSchema *kids[1] = { &cd };
  mk_schema(&lv, "u", NULL, NULL, 0, NULL, 0);
  mk_schema(&cd, "c", "d", NULL, 0, &lv, 0);
  mk_schema(&root, "+s", NULL, kids, 1, NULL, 0);
  mizu_arrow *a = NULL;

  /* a null level declines, the column named */
  const uint8_t lbm = 0x01;   /* level 1 null */
  const char levbytes[7] = { 'l', 'o', 'w', 'h', 'i', 'g', 'h' };
  const int32_t levoffs[3] = { 0, 3, 7 };
  ArrowArray dict;
  const void *dbufs[3] = { &lbm, levoffs, levbytes };
  mk_array(&dict, 2, 1, 0, 3, dbufs, NULL, NULL);
  const int8_t codes[2] = { 0, 1 };
  const void *cb[2] = { NULL, codes };
  ArrowArray a1, batch;
  ArrowArray *bk[1] = { &a1 };
  mk_array(&a1, 2, 0, 0, 2, cb, NULL, &dict);
  mk_array(&batch, 2, 0, 0, 1, NULL, bk, NULL);
  batch.n_children = 1;
  const ArrowArray *batches[1] = { &batch };
  assert(mizu_arrow_open(&a, &root, batches, 1, 0) == MIZU_ERR);
  assert(strstr(mizu_last_error_message(), "'d'") != NULL);
  assert(strstr(mizu_last_error_message(), "null level") != NULL);

  /* levels differing across batches decline */
  const int32_t levoffs2[3] = { 0, 3, 8 };
  const char levbytes2[8] = { 'l', 'o', 'w', 'h', 'i', 'g', 'h', '!' };
  ArrowArray dict1, dict2, b1, b2, c1, c2;
  ArrowArray *k1[1] = { &c1 };
  ArrowArray *k2[1] = { &c2 };
  const void *db1[3] = { NULL, levoffs, levbytes };
  const void *db2[3] = { NULL, levoffs2, levbytes2 };
  mk_array(&dict1, 2, 0, 0, 3, db1, NULL, NULL);
  mk_array(&dict2, 2, 0, 0, 3, db2, NULL, NULL);
  mk_array(&c1, 1, 0, 0, 2, cb, NULL, &dict1);
  mk_array(&c2, 1, 0, 0, 2, cb, NULL, &dict2);
  mk_array(&b1, 1, 0, 0, 1, NULL, k1, NULL);
  b1.n_children = 1;
  mk_array(&b2, 1, 0, 0, 1, NULL, k2, NULL);
  b2.n_children = 1;
  const ArrowArray *two[2] = { &b1, &b2 };
  assert(mizu_arrow_open(&a, &root, two, 2, 0) == MIZU_ERR);
  assert(strstr(mizu_last_error_message(), "differ across batches") !=
         NULL);

  /* an ordered dictionary declines */
  mk_schema(&cd, "c", "d", NULL, 0, &lv, MIZU_ARROW_FLAG_DICTIONARY_ORDERED);
  assert(mizu_arrow_open(&a, &root, batches, 1, 0) == MIZU_ERR);
  assert(strstr(mizu_last_error_message(), "pl.Categorical") != NULL);
  mk_schema(&cd, "c", "d", NULL, 0, &lv, 0);

  /* an out-of-range index declines */
  const int8_t bad[2] = { 0, 5 };
  const void *cbb[2] = { NULL, bad };
  ArrowArray c3, b3;
  ArrowArray *k3[1] = { &c3 };
  mk_array(&c3, 2, 0, 0, 2, cbb, NULL, &dict1);
  mk_array(&b3, 2, 0, 0, 1, NULL, k3, NULL);
  b3.n_children = 1;
  const ArrowArray *badbatch[1] = { &b3 };
  assert(mizu_arrow_open(&a, &root, badbatch, 1, 0) == MIZU_ERR);
  assert(strstr(mizu_last_error_message(), "outside the levels") != NULL);

  /* a non-string dictionary value declines */
  mk_schema(&lv, "i", NULL, NULL, 0, NULL, 0);
  assert(mizu_arrow_open(&a, &root, batches, 1, 0) == MIZU_ERR);
  assert(strstr(mizu_last_error_message(), "non-string") != NULL);
}

// Temporal columns -----------------------------------------------------------------

static void test_temporal(void) {
  ArrowSchema cd, ct, cn, cu, root;
  ArrowSchema *kids[4] = { &cd, &ct, &cn, &cu };
  mk_schema(&cd, "tdD", "d", NULL, 0, NULL, 0);
  mk_schema(&ct, "tsu:America/New_York", "t", NULL, 0, NULL, 0);
  mk_schema(&cn, "tss:", "n", NULL, 0, NULL, 0);
  mk_schema(&cu, "tDu", "u", NULL, 0, NULL, 0);
  mk_schema(&root, "+s", NULL, kids, 4, NULL, 0);

  const uint8_t bm4 = 0x0B;   /* element 2 null */
  const int32_t days[4] = { 18262, 0, 19000, 0 };
  const int64_t tss[4] = { 1700000000, 0, 1700003600, 0 };
  const int64_t dur[4] = { 90, 0, 0, 86400 };
  const void *db[2] = { &bm4, days };
  const void *tb[2] = { &bm4, tss };
  const void *nb[2] = { &bm4, tss };
  const void *ub[2] = { &bm4, dur };
  ArrowArray ad, at, an, au, batch;
  ArrowArray *bk[4] = { &ad, &at, &an, &au };
  mk_array(&ad, 4, 1, 0, 2, db, NULL, NULL);
  mk_array(&at, 4, 1, 0, 2, tb, NULL, NULL);
  mk_array(&an, 4, 1, 0, 2, nb, NULL, NULL);
  mk_array(&au, 4, 1, 0, 2, ub, NULL, NULL);
  mk_array(&batch, 4, 0, 0, 1, NULL, bk, NULL);
  batch.n_children = 4;
  const ArrowArray *batches[1] = { &batch };
  size_t total;
  mizu_arrow_warn warn;
  mizu_arrow *a;
  uint8_t *buf = frame_write(&root, batches, 1, 0, &total, &warn, &a);
  mizu_mizl_entry e;
  const uint64_t na_r = 0x7FF00000000007A2ULL;
  for (int i = 0; i < 4; i++) {
    assert(mizu_mizl_elem(buf, total, i, &e) == 0);
    assert(e.sexptype == MIZU_TYPE_REAL && e.attrs_size > 0);
    const double *v = (const double *) (buf + e.data_offset);
    uint64_t bits;
    memcpy(&bits, &v[2], 8);
    assert(bits == na_r);   /* NaT carries R's verbatim NA_real_ bits */
    blob_check(buf + e.data_offset + e.data_size - e.attrs_size,
               (size_t) e.attrs_size);
  }
  assert(mizu_mizl_elem(buf, total, 0, &e) == 0);
  const double *dv = (const double *) (buf + e.data_offset);
  assert(dv[0] == 18262.0);
  assert(mizu_mizl_elem(buf, total, 1, &e) == 0);
  const double *tv = (const double *) (buf + e.data_offset);
  assert(tv[0] == 1700.0 && tv[3] == 0.0);   /* tsu: microseconds */
  /* the tzone: "America/New_York" in the ts blob, "UTC" for the naive */
  const uint8_t *tsblob = buf + e.data_offset + e.data_size - e.attrs_size;
  assert(find_span(tsblob, (size_t) e.attrs_size, "America/New_York", 16) !=
         NULL);
  assert(mizu_mizl_elem(buf, total, 2, &e) == 0);
  const double *nv = (const double *) (buf + e.data_offset);
  assert(nv[0] == 1700000000.0);   /* tss: seconds */
  assert(mizu_mizl_elem(buf, total, 2, &e) == 0);
  const uint8_t *nblob = buf + e.data_offset + e.data_size - e.attrs_size;
  assert(find_span(nblob, (size_t) e.attrs_size, "UTC", 3) != NULL);
  /* duration[us]: 90 us -> 9e-5 s, 86400 us -> 864 s */
  assert(mizu_mizl_elem(buf, total, 3, &e) == 0);
  const double *uv = (const double *) (buf + e.data_offset);
  assert(uv[0] == 90.0 * 1e-6 && uv[3] == 86400.0 * 1e-6);
  const uint8_t *ublob = buf + e.data_offset + e.data_size - e.attrs_size;
  assert(find_span(ublob, (size_t) e.attrs_size, "difftime", 8) != NULL);
  free(buf);
  mizu_arrow_close(a);
}

static void test_temporal_ns(void) {
  /* tsn / tDn: the verbatim INT64 leaf with a capable peer word, the REAL
     fallback without */
  ArrowSchema ct, cu, root;
  ArrowSchema *kids[2] = { &ct, &cu };
  mk_schema(&ct, "tsn:America/New_York", "t", NULL, 0, NULL, 0);
  mk_schema(&cu, "tDn", "u", NULL, 0, NULL, 0);
  mk_schema(&root, "+s", NULL, kids, 2, NULL, 0);

  const uint8_t bm4 = 0x0B;   /* element 2 null */
  const int64_t tsv[4] = { 1700000000123456789LL, -3600000000001LL, 0, 0 };
  const int64_t dur[4] = { 90, 86400000000000LL, 0, 0 };
  const void *tb[2] = { &bm4, tsv };
  const void *ub[2] = { &bm4, dur };
  ArrowArray at, au, batch;
  ArrowArray *bk[2] = { &at, &au };
  mk_array(&at, 4, 1, 0, 2, tb, NULL, NULL);
  mk_array(&au, 4, 1, 0, 2, ub, NULL, NULL);
  mk_array(&batch, 4, 0, 0, 1, NULL, bk, NULL);
  batch.n_children = 2;
  const ArrowArray *batches[1] = { &batch };
  mizu_arrow_warn warn;
  mizu_arrow *a;
  mizu_mizl_entry e;

  /* the capable word: verbatim INT64 leaves, the tscale pair present */
  size_t total;
  uint8_t *buf = frame_write(&root, batches, 1, MIZU_CAP_MIZL_TSN, &total,
                             &warn, &a);
  for (int i = 0; i < 2; i++) {
    assert(mizu_mizl_elem(buf, total, i, &e) == 0);
    assert(e.sexptype == MIZU_TYPE_INT64 && e.attrs_size > 0);
    const int64_t *v = (const int64_t *) (buf + e.data_offset);
    assert(v[2] == MIZU_NA_INT64);   /* NaT at the null lane */
    const uint8_t *blob = buf + e.data_offset + e.data_size - e.attrs_size;
    assert(find_span(blob, (size_t) e.attrs_size, "tscale", 6) != NULL);
    assert(find_span(blob, (size_t) e.attrs_size, "ns", 2) != NULL);
    blob_check(blob, (size_t) e.attrs_size);
  }
  assert(mizu_mizl_elem(buf, total, 0, &e) == 0);
  const int64_t *tv = (const int64_t *) (buf + e.data_offset);
  assert(tv[0] == 1700000000123456789LL && tv[1] == -3600000000001LL);
  const uint8_t *tblob = buf + e.data_offset + e.data_size - e.attrs_size;
  assert(find_span(tblob, (size_t) e.attrs_size, "America/New_York", 16) !=
         NULL);
  assert(mizu_mizl_elem(buf, total, 1, &e) == 0);
  const int64_t *uv = (const int64_t *) (buf + e.data_offset);
  assert(uv[0] == 90 && uv[1] == 86400000000000LL);
  const uint8_t *ublob = buf + e.data_offset + e.data_size - e.attrs_size;
  assert(find_span(ublob, (size_t) e.attrs_size, "difftime", 8) != NULL);
  /* the validity tail rides as for any atomic leaf */
  int64_t aoff, asz, valid[2];
  assert(frame_check(buf, total, &aoff, &asz, valid) == 2);
  assert(valid[0] != 0 && valid[1] == 2);
  free(buf);
  mizu_arrow_close(a);

  /* a peer word short of the bit: the REAL fallback, the pair absent */
  buf = frame_write(&root, batches, 1, 0, &total, &warn, &a);
  const uint64_t na_r = 0x7FF00000000007A2ULL;
  for (int i = 0; i < 2; i++) {
    assert(mizu_mizl_elem(buf, total, i, &e) == 0);
    assert(e.sexptype == MIZU_TYPE_REAL && e.attrs_size > 0);
    const double *v = (const double *) (buf + e.data_offset);
    uint64_t bits;
    memcpy(&bits, &v[2], 8);
    assert(bits == na_r);
    const uint8_t *blob = buf + e.data_offset + e.data_size - e.attrs_size;
    assert(find_span(blob, (size_t) e.attrs_size, "tscale", 6) == NULL);
    blob_check(blob, (size_t) e.attrs_size);
  }
  assert(mizu_mizl_elem(buf, total, 0, &e) == 0);
  const double *dv = (const double *) (buf + e.data_offset);
  assert(dv[0] == (double) tsv[0] * 1e-9 && dv[1] == (double) tsv[1] * 1e-9);
  free(buf);
  mizu_arrow_close(a);
}

// Declines ------------------------------------------------------------------------------

static void test_declines(void) {
  ArrowSchema cx, cs, root;
  ArrowSchema *kids[2] = { &cx, &cs };
  mizu_arrow *a = NULL;
  const int32_t x[1] = { 1 };
  const void *xb[2] = { NULL, x };
  ArrowArray ax, as, batch;
  ArrowArray *bk[2] = { &ax, &as };
  const int32_t so[2] = { 0, 1 };
  const char sb[1] = { 'a' };
  const void *sbv[3] = { NULL, so, sb };
  mk_array(&ax, 1, 0, 0, 2, xb, NULL, NULL);
  mk_array(&as, 1, 0, 0, 3, sbv, NULL, NULL);
  mk_array(&batch, 1, 0, 0, 1, NULL, bk, NULL);
  batch.n_children = 2;
  const ArrowArray *batches[1] = { &batch };

  /* zero-column */
  mk_schema(&root, "+s", NULL, NULL, 0, NULL, 0);
  assert(mizu_arrow_open(&a, &root, NULL, 0, 0) == MIZU_ERR);
  assert(strstr(mizu_last_error_message(), "zero-column") != NULL);

  /* unnamed */
  mk_schema(&cx, "i", NULL, NULL, 0, NULL, 0);
  mk_schema(&cs, "u", "s", NULL, 0, NULL, 0);
  mk_schema(&root, "+s", NULL, kids, 2, NULL, 0);
  assert(mizu_arrow_open(&a, &root, batches, 1, 0) == MIZU_ERR);
  assert(strstr(mizu_last_error_message(), "unnamed") != NULL);

  /* duplicate */
  mk_schema(&cx, "i", "s", NULL, 0, NULL, 0);
  assert(mizu_arrow_open(&a, &root, batches, 1, 0) == MIZU_ERR);
  assert(strstr(mizu_last_error_message(), "duplicate") != NULL);

  /* an unsupported type names the column and format */
  mk_schema(&cx, "+l", "x", NULL, 1, NULL, 0);
  mk_schema(&cs, "u", "s", NULL, 0, NULL, 0);
  assert(mizu_arrow_open(&a, &root, batches, 1, 0) == MIZU_ERR);
  assert(strstr(mizu_last_error_message(), "'x'") != NULL);
  assert(strstr(mizu_last_error_message(), "'+l'") != NULL);
  assert(strstr(mizu_last_error_message(), "no portable home") != NULL);

  /* uint8 with nulls declines */
  mk_schema(&cx, "C", "x", NULL, 0, NULL, 0);
  const uint8_t u8[1] = { 1 };
  const uint8_t bm = 0x00;
  const void *ub[2] = { &bm, u8 };
  mk_array(&ax, 1, 1, 0, 2, ub, NULL, NULL);
  assert(mizu_arrow_open(&a, &root, batches, 1, 0) == MIZU_ERR);
  assert(strstr(mizu_last_error_message(), "uint8") != NULL);

  /* a batch whose column count differs from the schema */
  ArrowArray shy;
  mk_array(&shy, 1, 0, 0, 1, NULL, NULL, NULL);
  shy.n_children = 1;
  const ArrowArray *bb[1] = { &shy };
  assert(mizu_arrow_open(&a, &root, bb, 1, 0) == MIZU_ERR);
  assert(strstr(mizu_last_error_message(), "column count") != NULL);

  /* a batch whose columns differ in length */
  mk_schema(&cx, "i", "x", NULL, 0, NULL, 0);
  mk_array(&ax, 2, 0, 0, 2, xb, NULL, NULL);
  batch.n_children = 2;
  assert(mizu_arrow_open(&a, &root, batches, 1, 0) == MIZU_ERR);
  assert(strstr(mizu_last_error_message(), "differ in length") != NULL);
}

// The remote leaf --------------------------------------------------------------------

/* A referenced one-column int32 frame; nulls per the argument. */
static uint8_t *ref_region(int with_nulls, size_t *total_out) {
  ArrowSchema cx, root;
  ArrowSchema *kids[1] = { &cx };
  mk_schema(&cx, "i", "x", NULL, 0, NULL, 0);
  mk_schema(&root, "+s", NULL, kids, 1, NULL, 0);
  const uint8_t bm = 0x05;
  const int32_t x[3] = { 7, 8, 9 };
  const void *xb[2] = { with_nulls ? &bm : NULL, x };
  ArrowArray ax, batch;
  ArrowArray *bk[1] = { &ax };
  mk_array(&ax, 3, with_nulls, 0, 2, xb, NULL, NULL);
  mk_array(&batch, 3, 0, 0, 1, NULL, bk, NULL);
  batch.n_children = 1;
  const ArrowArray *batches[1] = { &batch };
  mizu_arrow_warn warn;
  mizu_arrow *a;
  uint8_t *buf = frame_write(&root, batches, 1, 0, total_out, &warn, &a);
  mizu_arrow_close(a);
  return buf;
}

static void test_remote(void) {
  /* the outgoing frame: col 0 the remote leaf, col 1 a local int32 */
  size_t reftotal;
  uint8_t *ref = ref_region(0, &reftotal);
  ArrowSchema cx, cy, root;
  ArrowSchema *kids[2] = { &cx, &cy };
  mk_schema(&cx, "i", "x", NULL, 0, NULL, 0);
  mk_schema(&cy, "i", "y", NULL, 0, NULL, 0);
  mk_schema(&root, "+s", NULL, kids, 2, NULL, 0);
  const uint8_t bm = 0x05;
  const int32_t x[3] = { 7, 8, 9 };
  const int32_t y[3] = { 1, 2, 3 };
  const void *xb[2] = { NULL, x };
  const void *yb[2] = { &bm, y };
  ArrowArray ax, ay, batch;
  ArrowArray *bk[2] = { &ax, &ay };
  mk_array(&ax, 3, 0, 0, 2, xb, NULL, NULL);
  mk_array(&ay, 3, 1, 0, 2, yb, NULL, NULL);
  mk_array(&batch, 3, 0, 0, 1, NULL, bk, NULL);
  batch.n_children = 2;
  const ArrowArray *batches[1] = { &batch };

  mizu_arrow *a = NULL;
  assert(mizu_arrow_open(&a, &root, batches, 1, 0) == MIZU_OK);
  const char *ident = "/mizu_testref";
  assert(mizu_arrow_remote(a, 0, ident, strlen(ident), ref, reftotal,
                           0) == 0);
  /* a torn referenced tree leaves the column local */
  uint8_t garbage[128];
  memset(garbage, 0xAB, sizeof garbage);
  assert(mizu_arrow_remote(a, 1, ident, strlen(ident), garbage,
                           sizeof garbage, 0) == -1);

  size_t total = mizu_arrow_size(a);
  uint8_t *buf = calloc(1, total);
  mizu_arrow_warn warn;
  memset(&warn, 0, sizeof warn);
  mizu_arrow_write(a, buf, &warn);
  mizu_mizl_entry e;
  /* col 0: tag 33, the identifier span, the referenced leaf's claims */
  assert(mizu_mizl_elem(buf, total, 0, &e) == 0);
  assert(e.sexptype == 33 && e.length == 3);
  assert(e.data_size == (int64_t) strlen("/mizu_testref[1]"));
  assert(memcmp(buf + e.data_offset, "/mizu_testref[1]",
                (size_t) e.data_size) == 0);
  assert(e.valid[0] == 0 && e.valid[1] == -1);   /* the known-NA-free claim */
  /* the local column's bitmap rides; the remote contributes no count */
  int64_t aoff, asz, valid[2];
  assert(frame_check(buf, total, &aoff, &asz, valid) == 2);
  assert(valid[1] == 1);
  assert(mizu_mizl_elem(buf, total, 1, &e) == 0 && e.valid[1] == 1);
  /* REFHELD on the referenced region */
  uint32_t flags;
  memcpy(&flags, ref + 28, 4);
  assert((flags & MIZU_ZC_FLAG_REFHELD) != 0);
  free(buf);
  mizu_arrow_close(a);

  /* an all-remote frame claiming known-NA-free: the {0, -1} collapse —
     no tail bytes at all */
  assert(mizu_arrow_open(&a, &root, batches, 1, 0) == MIZU_OK);
  assert(mizu_arrow_remote(a, 0, ident, strlen(ident), ref, reftotal,
                           0) == 0);
  assert(mizu_arrow_remote(a, 1, ident, strlen(ident), ref, reftotal,
                           0) == 0);
  size_t t2 = mizu_arrow_size(a);
  uint8_t *b2 = calloc(1, t2);
  mizu_arrow_write(a, b2, &warn);
  assert(frame_check(b2, t2, &aoff, &asz, valid) == 2);
  assert(valid[0] == 0 && valid[1] == -1);
  /* the root blob is the last thing in the region */
  assert(t2 == (size_t) MIZU_ALIGN64((size_t) aoff + (size_t) asz));
  free(b2);
  mizu_arrow_close(a);

  /* a remote claim short of known-NA-free: the table rides with the
     claim alone, the header total 0 */
  size_t ref2total;
  uint8_t *ref2 = ref_region(1, &ref2total);
  assert(mizu_arrow_open(&a, &root, batches, 1, 0) == MIZU_OK);
  assert(mizu_arrow_remote(a, 0, ident, strlen(ident), ref2, ref2total,
                           0) == 0);
  assert(mizu_arrow_remote(a, 1, ident, strlen(ident), ref2, ref2total,
                           0) == 0);
  size_t t3 = mizu_arrow_size(a);
  uint8_t *b3 = calloc(1, t3);
  mizu_arrow_write(a, b3, &warn);
  assert(frame_check(b3, t3, &aoff, &asz, valid) == 2);
  assert(valid[0] != 0 && valid[1] == 0);
  assert(mizu_mizl_elem(b3, t3, 0, &e) == 0);
  assert(e.valid[0] == 0 && e.valid[1] == 0);
  free(b3);
  mizu_arrow_close(a);
  free(ref2);
  free(ref);
}

// The overrides ----------------------------------------------------------------------

static void test_root_blob_override(void) {
  ArrowSchema cx, root;
  ArrowSchema *kids[1] = { &cx };
  mk_schema(&cx, "i", "x", NULL, 0, NULL, 0);
  mk_schema(&root, "+s", NULL, kids, 1, NULL, 0);
  const int32_t x[1] = { 1 };
  const void *xb[2] = { NULL, x };
  ArrowArray ax, batch;
  ArrowArray *bk[1] = { &ax };
  mk_array(&ax, 1, 0, 0, 2, xb, NULL, NULL);
  mk_array(&batch, 1, 0, 0, 1, NULL, bk, NULL);
  batch.n_children = 1;
  const ArrowArray *batches[1] = { &batch };

  /* a caller-built blob: dict {names = "x", class = "tbl"} */
  uint8_t blob[128];
  size_t n = 0;
  n += mizu_ix_put_header(blob + n);
  n += mizu_ix_put_dict_begin(blob + n, 2);
  n += mizu_ix_put_key(blob + n, "names", 5);
  n += mizu_ix_put_str(blob + n, "x", 1);
  n += mizu_ix_put_key(blob + n, "class", 5);
  n += mizu_ix_put_str(blob + n, "tbl", 3);

  mizu_arrow *a = NULL;
  assert(mizu_arrow_open(&a, &root, batches, 1, 0) == MIZU_OK);
  mizu_arrow_root_blob(a, blob, n);
  size_t total = mizu_arrow_size(a);
  uint8_t *buf = calloc(1, total);
  mizu_arrow_warn warn;
  memset(&warn, 0, sizeof warn);
  mizu_arrow_write(a, buf, &warn);
  int64_t aoff, asz, valid[2];
  assert(frame_check(buf, total, &aoff, &asz, valid) == 1);
  assert(asz == (int64_t) n);
  assert(memcmp(buf + aoff, blob, n) == 0);
  blob_check(buf + aoff, (size_t) asz);
  free(buf);
  mizu_arrow_close(a);
}

static void test_tz_override(void) {
  ArrowSchema ct, root;
  ArrowSchema *kids[1] = { &ct };
  mk_schema(&ct, "tsu:", "t", NULL, 0, NULL, 0);
  mk_schema(&root, "+s", NULL, kids, 1, NULL, 0);
  const int64_t ts[1] = { 1700000000 };
  const void *tb[2] = { NULL, ts };
  ArrowArray at, batch;
  ArrowArray *bk[1] = { &at };
  mk_array(&at, 1, 0, 0, 2, tb, NULL, NULL);
  mk_array(&batch, 1, 0, 0, 1, NULL, bk, NULL);
  batch.n_children = 1;
  const ArrowArray *batches[1] = { &batch };
  mizu_arrow *a = NULL;
  assert(mizu_arrow_open(&a, &root, batches, 1, 0) == MIZU_OK);
  mizu_arrow_tz(a, 0, "");   /* the same-language verbatim form */
  size_t total = mizu_arrow_size(a);
  uint8_t *buf = calloc(1, total);
  mizu_arrow_warn warn;
  memset(&warn, 0, sizeof warn);
  mizu_arrow_write(a, buf, &warn);
  mizu_mizl_entry e;
  assert(mizu_mizl_elem(buf, total, 0, &e) == 0);
  const uint8_t *blob = buf + e.data_offset + e.data_size - e.attrs_size;
  assert(find_span(blob, (size_t) e.attrs_size, "UTC", 3) == NULL);
  blob_check(blob, (size_t) e.attrs_size);
  free(buf);
  mizu_arrow_close(a);
  /* and the classified default writes "UTC" */
  assert(mizu_arrow_open(&a, &root, batches, 1, 0) == MIZU_OK);
  total = mizu_arrow_size(a);
  buf = calloc(1, total);
  mizu_arrow_write(a, buf, &warn);
  assert(mizu_mizl_elem(buf, total, 0, &e) == 0);
  blob = buf + e.data_offset + e.data_size - e.attrs_size;
  assert(find_span(blob, (size_t) e.attrs_size, "UTC", 3) != NULL);
  free(buf);
  mizu_arrow_close(a);
}

// The stage ------------------------------------------------------------------------------

static mizu_binding bytesb;
static mizu_arrow *stage_arrow;
static mizu_arrow_warn stage_warn;
static int stage_rc;
static mizu_slot_hdr stage_hdr;
static uint8_t stage_name[MIZU_NAME_MAX];

static int arrow_stage_fn(void *obj, mizu_slot_hdr *hdr, uint8_t *payload,
                          uint32_t inline_max, mizu_handle *h, void *ctx) {
  (void) obj;
  (void) inline_max;
  (void) ctx;
  stage_rc = mizu_arrow_stage(h, stage_arrow, hdr, payload, &stage_warn);
  if (stage_rc != MIZU_OK) return 1;
  stage_hdr = *hdr;
  memcpy(stage_name, payload, hdr->len);
  return 0;
}

static void test_stage(void) {
  ArrowSchema ci, cs, root;
  ArrowSchema *kids[2] = { &ci, &cs };
  mk_schema(&ci, "i", "x", NULL, 0, NULL, 0);
  mk_schema(&cs, "u", "s", NULL, 0, NULL, 0);
  mk_schema(&root, "+s", NULL, kids, 2, NULL, 0);
  const uint8_t bm = 0x05;
  const int32_t x[3] = { 1, -2, 999 };
  const int32_t so[4] = { 0, 1, 1, 3 };
  const char sb[3] = { 'a', 'b', 'c' };
  const void *xb[2] = { &bm, x };
  const void *sbv[3] = { &bm, so, sb };
  ArrowArray ax, as, batch;
  ArrowArray *bk[2] = { &ax, &as };
  mk_array(&ax, 3, 1, 0, 2, xb, NULL, NULL);
  mk_array(&as, 3, 1, 0, 3, sbv, NULL, NULL);
  mk_array(&batch, 3, 0, 0, 1, NULL, bk, NULL);
  batch.n_children = 2;
  const ArrowArray *batches[1] = { &batch };

  mizu_binding b;
  mizu_binding_init(&b);
  mizu_binding_bytes(&bytesb);
  b = bytesb;
  b.stage = arrow_stage_fn;
  b.ident = MIZU_IDENT(MIZU_LANG_BYTES, 0);

  mizu_channel_opts opts;
  mizu_channel_opts_init(&opts);
  opts.capacity = 8;
  opts.slot_size = 256;
  opts.arena_size = 64 << 10;
  mizu_channel *host = NULL, *peer = NULL;
  assert(mizu_channel_create(&host, &opts, &b) == MIZU_OK);
  char token[64];
  assert(mizu_channel_token(host, token, sizeof(token)) == MIZU_OK);
  assert(mizu_channel_attach(&peer, token, &bytesb) == MIZU_OK);
  assert(mizu_channel_ready_set(peer) == MIZU_OK);
  assert(mizu_channel_ready_wait(host, 5000) == MIZU_OK);

  /* caps short of the conjunction (MIZS missing, a string column
     present): the stage declines, nothing claimed */
  assert(mizu_arrow_open(&stage_arrow, &root, batches, 1,
                         MIZU_CAP_MIZL | MIZU_CAP_ATTRS) == MIZU_OK);
  assert(mizu_channel_send(host, &(mizu_bytes) { NULL, 0 }) == MIZU_ERR);
  assert(stage_rc == MIZU_ERR);
  assert(mizu_handle_keep_out((mizu_handle *) host) == 0);
  mizu_arrow_close(stage_arrow);

  /* the full conjunction: staged SHM_VEC, one zc keeper, the bytes equal
     the raw write */
  const uint32_t caps = MIZU_CAP_MIZL | MIZU_CAP_ATTRS | MIZU_CAP_MIZS;
  assert(mizu_arrow_open(&stage_arrow, &root, batches, 1, caps) ==
         MIZU_OK);
  size_t total = mizu_arrow_size(stage_arrow);
  uint8_t *exp = calloc(1, total);
  mizu_arrow_warn w2;
  memset(&w2, 0, sizeof w2);
  mizu_arrow_write(stage_arrow, exp, &w2);
  assert(mizu_channel_send(host, &(mizu_bytes) { NULL, 0 }) == MIZU_OK);
  assert(stage_rc == MIZU_OK);
  assert(stage_hdr.kind == MIZU_KIND_SHM_VEC);
  assert((stage_hdr.aux & 0xFFu) == MIZU_TYPE_VEC);
  assert(stage_warn.n_range == 0 && stage_warn.n_intmin == 0);
  char rname[MIZU_NAME_MAX + 1];
  memset(rname, 0, sizeof rname);
  memcpy(rname, stage_name, stage_hdr.len);
  mizu_shm *got = NULL;
  assert(mizu_shm_open_view(&got, rname) == MIZU_OK);
  assert(mizu_shm_size(got) >= total);
  /* the zc band [24-31]: the producer loan plus this open's counted add */
  assert(memcmp(mizu_shm_addr(got), exp, 24) == 0);
  assert(memcmp((const uint8_t *) mizu_shm_addr(got) + 32, exp + 32,
                total - 32) == 0);
  const uint8_t *rc = (const uint8_t *) mizu_shm_addr(got) + 24;
  uint32_t rcword;
  memcpy(&rcword, rc, 4);
  assert(rcword == 2);   /* the producer loan + this view's */
  mizu_shm_close(got, 0);
  free(exp);

  mizu_channel_destroy(host);
  mizu_channel_destroy(peer);
  mizu_arrow_close(stage_arrow);
}

static void test_col_info(void) {
  ArrowSchema lv, cd, ct, root;
  ArrowSchema *kids[2] = { &cd, &ct };
  mk_schema(&lv, "u", NULL, NULL, 0, NULL, 0);
  mk_schema(&cd, "s", "d", NULL, 0, &lv, 0);
  mk_schema(&ct, "tsm:UTC", "t", NULL, 0, NULL, 0);
  mk_schema(&root, "+s", NULL, kids, 2, NULL, 0);
  const char levbytes[7] = { 'l', 'o', 'w', 'h', 'i', 'g', 'h' };
  const int32_t levoffs[3] = { 0, 3, 7 };
  ArrowArray dict, ai, at, batch;
  const void *db[3] = { NULL, levoffs, levbytes };
  mk_array(&dict, 2, 0, 0, 3, db, NULL, NULL);
  const int16_t codes[1] = { 1 };
  const int64_t tsv[1] = { 1000 };
  const void *cb[2] = { NULL, codes };
  const void *tb[2] = { NULL, tsv };
  ArrowArray *bk[2] = { &ai, &at };
  mk_array(&ai, 1, 0, 0, 2, cb, NULL, &dict);
  mk_array(&at, 1, 0, 0, 2, tb, NULL, NULL);
  mk_array(&batch, 1, 0, 0, 1, NULL, bk, NULL);
  batch.n_children = 2;
  const ArrowArray *batches[1] = { &batch };
  mizu_arrow *a = NULL;
  assert(mizu_arrow_open(&a, &root, batches, 1, 0) == MIZU_OK);
  mizu_arrow_col ci;
  mizu_arrow_col_info(a, 0, &ci);
  assert(ci.kind == MIZU_ARROW_DICT && ci.idx_w == 2 && ci.idx_signed);
  assert(ci.nlev == 2 && ci.lev_offs[2] == 7 && ci.lev_blen == 7);
  assert(memcmp(ci.lev_bytes, "lowhigh", 7) == 0);
  mizu_arrow_col_info(a, 1, &ci);
  assert(ci.kind == MIZU_ARROW_TS && ci.ts_scale == 1e-3);
  assert(strcmp(ci.tz, "UTC") == 0);
  mizu_arrow_close(a);
}

int main(void) {
  test_frame_a();
  test_cvt_types();
  test_intmin();
  test_zero_rows();
  test_chunked_and_offsets();
  test_null_span_collapse();
  test_null_count_forms();
  test_nan_vs_na();
  test_string_forms();
  test_dicts();
  test_dict_declines();
  test_temporal();
  test_temporal_ns();
  test_declines();
  test_remote();
  test_root_blob_override();
  test_tz_override();
  test_stage();
  test_col_info();
  printf("test_arrow: OK\n");
  return 0;
}
