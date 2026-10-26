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
  const uint64_t u64[3] = { 9223372036854775808ULL, 0, 9007199254740993ULL };   /* > INT64_MAX; > 2^53 */
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
                         MIZU_TYPE_REAL, MIZU_TYPE_INT64, MIZU_TYPE_INT64,
                         MIZU_TYPE_REAL, MIZU_TYPE_REAL };
  mizu_mizl_entry e;
  for (int i = 0; i < 11; i++) {
    assert(mizu_mizl_elem(buf, total, i, &e) == 0);
    assert(e.sexptype == tags[i] && e.length == 3);
    if (i == 4) assert(e.valid[0] == 0 && e.valid[1] == -1);
    else assert(e.valid[1] == 1);
  }
  /* spot values: bool {1, NA, 1}, u16 widened, u64 past INT64_MAX -> the
     int64 NA sentinel, a past-2^53 lane exact */
  assert(mizu_mizl_elem(buf, total, 0, &e) == 0);
  const int32_t *lgl = (const int32_t *) (buf + e.data_offset);
  assert(lgl[0] == 1 && lgl[1] == MIZU_NA_INT32 && lgl[2] == 1);
  assert(mizu_mizl_elem(buf, total, 3, &e) == 0);
  const int32_t *w16 = (const int32_t *) (buf + e.data_offset);
  assert(w16[0] == 60000 && w16[1] == MIZU_NA_INT32 && w16[2] == 7);
  assert(mizu_mizl_elem(buf, total, 4, &e) == 0);
  assert(memcmp(buf + e.data_offset, u8, 3) == 0);
  assert(mizu_mizl_elem(buf, total, 8, &e) == 0);
  const int64_t *iu64 = (const int64_t *) (buf + e.data_offset);
  assert(iu64[0] == MIZU_NA_INT64);   /* past INT64_MAX: the NA sentinel */
  assert(iu64[2] == 9007199254740993LL);   /* past 2^53 converts exact */
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

static void test_intmin_unmasked(void) {
  /* the null-free (unmasked) path counts genuine sentinels exactly like
     the masked one: the value crosses verbatim and reads as NA in R */
  ArrowSchema ci, root;
  ArrowSchema *kids[1] = { &ci };
  mk_schema(&ci, "i", "x", NULL, 0, NULL, 0);
  mk_schema(&root, "+s", NULL, kids, 1, NULL, 0);
  const int32_t x[3] = { 1, INT32_MIN, 7 };
  const void *xb[2] = { NULL, x };
  ArrowArray ax, batch;
  ArrowArray *bk[1] = { &ax };
  mk_array(&ax, 3, 0, 0, 2, xb, NULL, NULL);
  mk_array(&batch, 3, 0, 0, 1, NULL, bk, NULL);
  batch.n_children = 1;
  const ArrowArray *batches[1] = { &batch };
  size_t total;
  mizu_arrow_warn warn;
  mizu_arrow *a;
  uint8_t *buf = frame_write(&root, batches, 1, 0, &total, &warn, &a);
  assert(warn.n_intmin == 1);
  mizu_mizl_entry e;
  assert(mizu_mizl_elem(buf, total, 0, &e) == 0);
  const int32_t *v = (const int32_t *) (buf + e.data_offset);
  assert(v[0] == 1 && v[1] == INT32_MIN && v[2] == 7);
  free(buf);
  mizu_arrow_close(a);
}

static void test_masked_unaligned(void) {
  /* the masked convert's full-word branch scans the written lanes by
     memcpy: the inline 'I' column body lands at an unaligned stream
     offset (C-15) — 70 rows with the one null at row 66, so lanes
     0..63 are an all-valid word */
  ArrowSchema ci, root;
  ArrowSchema *kids[1] = { &ci };
  mk_schema(&ci, "i", "x", NULL, 0, NULL, 0);
  mk_schema(&root, "+s", NULL, kids, 1, NULL, 0);
  uint8_t bm[9];
  memset(bm, 0xFF, sizeof bm);
  bm[66 >> 3] &= (uint8_t) ~(1u << (66 & 7));
  int32_t x[70];
  for (int i = 0; i < 70; i++) x[i] = i == 5 ? INT32_MIN : i;
  const void *xb[2] = { bm, x };
  ArrowArray ax, batch;
  ArrowArray *bk[1] = { &ax };
  mk_array(&ax, 70, 1, 0, 2, xb, NULL, NULL);
  mk_array(&batch, 70, 0, 0, 1, NULL, bk, NULL);
  batch.n_children = 1;
  const ArrowArray *batches[1] = { &batch };
  mizu_arrow *a = NULL;
  assert(mizu_arrow_open(&a, &root, batches, 1, 0) == MIZU_OK);
  mizu_arrow_warn warn;
  memset(&warn, 0, sizeof warn);
  size_t n = mizu_arrow_write_ix(a, NULL, &warn);
  uint8_t *buf = malloc(n);
  assert(buf != NULL);
  mizu_arrow_write_ix(a, buf, &warn);
  assert(warn.n_intmin == 1);
  blob_check(buf, n);
  free(buf);
  mizu_arrow_close(a);

  /* int64: the same on the i64 lanes */
  mk_schema(&ci, "l", "x", NULL, 0, NULL, 0);
  int64_t y[70];
  for (int i = 0; i < 70; i++) y[i] = i == 5 ? INT64_MIN : i;
  const void *yb[2] = { bm, y };
  mk_array(&ax, 70, 1, 0, 2, yb, NULL, NULL);
  assert(mizu_arrow_open(&a, &root, batches, 1, 0) == MIZU_OK);
  memset(&warn, 0, sizeof warn);
  n = mizu_arrow_write_ix(a, NULL, &warn);
  buf = malloc(n);
  assert(buf != NULL);
  mizu_arrow_write_ix(a, buf, &warn);
  assert(warn.n_int64 == 1);
  blob_check(buf, n);
  free(buf);
  mizu_arrow_close(a);
}

static void test_unaligned_buffers(void) {
  /* the read paths' alignment discipline (cvt_run's, applied to the
     string/dict/date/timestamp walks): every Arrow buffer sits at an odd
     address — the gathers read exactly the aligned case's values */
  ArrowSchema cs, cu, cd, cdt, cts, cv, lv, root;
  ArrowSchema *kids[6] = { &cs, &cu, &cd, &cdt, &cts, &cv };
  mk_schema(&cs, "u", "s", NULL, 0, NULL, 0);
  mk_schema(&cu, "U", "u", NULL, 0, NULL, 0);
  mk_schema(&lv, "u", NULL, NULL, 0, NULL, 0);
  mk_schema(&cd, "i", "d", NULL, 0, &lv, 0);
  mk_schema(&cdt, "tdD", "dt", NULL, 0, NULL, 0);
  mk_schema(&cts, "tss:", "ts", NULL, 0, NULL, 0);
  mk_schema(&cv, "vu", "v", NULL, 0, NULL, 0);
  mk_schema(&root, "+s", NULL, kids, 6, NULL, 0);

  /* every typed buffer is forced to an odd address (the pad byte covers
     the even case; the odd case already is one) */
#define ODD(p) ((void *) ((uintptr_t) (p) | 1u))
  const int32_t so[5] = { 0, 1, 3, 6, 7 };
  uint8_t so1[1 + sizeof so];
  uint8_t *sop = ODD(so1);
  memcpy(sop, so, sizeof so);
  const uint8_t ubm = 0x0D;   /* u: element 1 null */
  const int64_t lo[5] = { 0, 3, 3, 5, 9 };
  uint8_t lo1[1 + sizeof lo];
  uint8_t *lop = ODD(lo1);
  memcpy(lop, lo, sizeof lo);
  const int32_t lvoffs[3] = { 0, 3, 7 };
  uint8_t lv1[1 + sizeof lvoffs];
  uint8_t *lvp = ODD(lv1);
  memcpy(lvp, lvoffs, sizeof lvoffs);
  const int32_t dc[4] = { 0, 1, 1, 0 };
  uint8_t dc1[1 + sizeof dc];
  uint8_t *dcp = ODD(dc1);
  memcpy(dcp, dc, sizeof dc);
  const int32_t dd[4] = { 19723, 0, -1, 18993 };
  uint8_t dd1[1 + sizeof dd];
  uint8_t *ddp = ODD(dd1);
  memcpy(ddp, dd, sizeof dd);
  const int64_t tc[4] = { 1700000000LL, 0, -86400, 1 };
  uint8_t tc1[1 + sizeof tc];
  uint8_t *tcp = ODD(tc1);
  memcpy(tcp, tc, sizeof tc);
  /* four string_view elements naming one 20-byte span, the sizes array
     misaligned */
  uint8_t vws[4 * 16];
  memset(vws, 0, sizeof vws);
  char vdata[20];
  memset(vdata, 'z', sizeof vdata);
  for (int i = 0; i < 4; i++) {
    int32_t vl = 20, bi = 0, bo = 0;
    memcpy(vws + 16 * i, &vl, 4);
    memcpy(vws + 16 * i + 8, &bi, 4);
    memcpy(vws + 16 * i + 12, &bo, 4);
  }
  int64_t vsz = 20;
  uint8_t vs1[1 + 8];
  uint8_t *vsp = ODD(vs1);
  memcpy(vsp, &vsz, 8);

  const void *sb[3] = { NULL, sop, "abbcccd" };
  const void *ub[3] = { &ubm, lop, "aaabbdddd" };
  const void *db[3] = { NULL, lvp, "lowhigh" };
  const void *cb[2] = { NULL, dcp };
  const void *dtb[2] = { NULL, ddp };
  const void *tb[2] = { NULL, tcp };
  const void *vb[4] = { NULL, vws, vdata, vsp };
  ArrowArray as, au, adict, ad, adt, ats, av, batch;
  ArrowArray *bk[6] = { &as, &au, &ad, &adt, &ats, &av };
  mk_array(&as, 4, 0, 0, 3, sb, NULL, NULL);
  mk_array(&au, 4, 1, 0, 3, ub, NULL, NULL);
  mk_array(&adict, 2, 0, 0, 3, db, NULL, NULL);
  mk_array(&ad, 4, 0, 0, 2, cb, NULL, &adict);
  mk_array(&adt, 4, 0, 0, 2, dtb, NULL, NULL);
  mk_array(&ats, 4, 0, 0, 2, tb, NULL, NULL);
  mk_array(&av, 4, 0, 0, 4, vb, NULL, NULL);
  mk_array(&batch, 4, 0, 0, 1, NULL, bk, NULL);
  batch.n_children = 6;
  const ArrowArray *batches[1] = { &batch };

  size_t total;
  mizu_arrow_warn warn;
  mizu_arrow *a;
  uint8_t *buf = frame_write(&root, batches, 1, 0, &total, &warn, &a);
  mizu_mizl_entry e;
  mizu_mizs_geom g = mizu_mizs_geometry(4);
  /* s: the null-free span walk and rebase writer */
  assert(mizu_mizl_elem(buf, total, 0, &e) == 0);
  const uint8_t *blk = buf + e.data_offset;
  const int64_t *offs = (const int64_t *) (blk + g.offsets);
  assert(offs[0] == 0 && offs[1] == 1 && offs[2] == 3 && offs[3] == 6 &&
         offs[4] == 7);
  assert(memcmp(blk + g.data, "abbcccd", 7) == 0);
  /* u: the per-element str_at gather, the null collapsing to zero span */
  assert(mizu_mizl_elem(buf, total, 1, &e) == 0);
  blk = buf + e.data_offset;
  offs = (const int64_t *) (blk + g.offsets);
  assert(offs[0] == 0 && offs[1] == 3 && offs[2] == 3 && offs[3] == 5 &&
         offs[4] == 9);
  assert(memcmp(blk + g.data, "aaabbdddd", 9) == 0);
  assert(blk[0] == 0xFD);
  /* d: the index bounds vet and gather */
  assert(mizu_mizl_elem(buf, total, 2, &e) == 0);
  const int32_t *codes = (const int32_t *) (buf + e.data_offset);
  assert(codes[0] == 1 && codes[1] == 2 && codes[2] == 2 && codes[3] == 1);
  const uint8_t *blob = buf + e.data_offset + e.data_size - e.attrs_size;
  assert(find_span(blob, (size_t) e.attrs_size, "low", 3) != NULL);
  assert(find_span(blob, (size_t) e.attrs_size, "high", 4) != NULL);
  /* dt / ts: the date and timestamp element loads */
  assert(mizu_mizl_elem(buf, total, 3, &e) == 0);
  const double *ddays = (const double *) (buf + e.data_offset);
  assert(ddays[0] == 19723.0 && ddays[1] == 0.0 && ddays[2] == -1.0 &&
         ddays[3] == 18993.0);
  assert(mizu_mizl_elem(buf, total, 4, &e) == 0);
  const double *secs = (const double *) (buf + e.data_offset);
  assert(secs[0] == 1.7e9 && secs[1] == 0.0 && secs[2] == -86400.0 &&
         secs[3] == 1.0);
  /* v: the variadic sizes array bounds the spans */
  assert(mizu_mizl_elem(buf, total, 5, &e) == 0);
  blk = buf + e.data_offset;
  offs = (const int64_t *) (blk + g.offsets);
  assert(offs[0] == 0 && offs[1] == 20 && offs[2] == 40 && offs[3] == 60 &&
         offs[4] == 80);
  assert(memcmp(blk + g.data, vdata, 20) == 0);
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
  const int64_t vsizes[1] = { (int64_t) sizeof longs };
  const void *ubufs[3] = { &bm, uo, ub };
  const void *vbufs[4] = { &bm, views, longs, vsizes };
  ArrowArray au, av, b1;
  ArrowArray *k1[2] = { &au, &av };
  mk_array(&au, 2, 0, 0, 3, ubufs, NULL, NULL);
  mk_array(&av, 2, 0, 0, 4, vbufs, NULL, NULL);
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

static void test_strview_buffer_bound(void) {
  /* a span past the declared size is malformed, as is a buffer index
     past the buffer array; a view naming the trailing slot proves a
     no-sizes export (nanoarrow's nested or standalone form) and crosses
     unbounded */
  ArrowSchema cv, root;
  ArrowSchema *kids[1] = { &cv };
  mk_schema(&cv, "vu", "v", NULL, 0, NULL, 0);
  mk_schema(&root, "+s", NULL, kids, 1, NULL, 0);
  uint8_t views[16];
  memset(views, 0, sizeof views);
  char data[20];
  memset(data, 'z', sizeof data);
  unsigned char sizes[32];
  memset(sizes, 0, sizeof sizes);
  int64_t s0 = (int64_t) sizeof data;
  memcpy(sizes, &s0, 8);
  int32_t l = 20, bi = 0, bo = 0;
  memcpy(views, &l, 4);
  memcpy(views + 8, &bi, 4);
  memcpy(views + 12, &bo, 4);
  const void *vbufs[4] = { NULL, views, data, sizes };
  ArrowArray av, batch;
  ArrowArray *bk[1] = { &av };
  mk_array(&av, 1, 0, 0, 4, vbufs, NULL, NULL);
  mk_array(&batch, 1, 0, 0, 1, NULL, bk, NULL);
  batch.n_children = 1;
  const ArrowArray *batches[1] = { &batch };
  mizu_arrow *a = NULL;

  bi = 2;   /* past the buffer array */
  memcpy(views + 8, &bi, 4);
  assert(mizu_arrow_open(&a, &root, batches, 1, 0) == MIZU_ERR);
  assert(strstr(mizu_last_error_message(), "malformed") != NULL);

  bi = 0;
  bo = 8;   /* 8 + 20 past the declared 20 */
  memcpy(views + 8, &bi, 4);
  memcpy(views + 12, &bo, 4);
  assert(mizu_arrow_open(&a, &root, batches, 1, 0) == MIZU_ERR);
  assert(strstr(mizu_last_error_message(), "malformed") != NULL);

  bo = 0;   /* in bounds: opens and gathers the span */
  memcpy(views + 12, &bo, 4);
  size_t total;
  mizu_arrow_warn warn;
  uint8_t *buf = frame_write(&root, batches, 1, 0, &total, &warn, &a);
  mizu_mizl_entry e;
  assert(mizu_mizl_elem(buf, total, 0, &e) == 0);
  mizu_mizs_geom g = mizu_mizs_geometry(1);
  const int64_t *offs = (const int64_t *) (buf + e.data_offset + g.offsets);
  assert(offs[0] == 0 && offs[1] == 20);
  assert(memcmp(buf + e.data_offset + g.data, data, sizeof data) == 0);
  free(buf);
  mizu_arrow_close(a);

  bi = 1;   /* the trailing slot: the no-sizes tell, crosses unbounded */
  memcpy(views + 8, &bi, 4);
  buf = frame_write(&root, batches, 1, 0, &total, &warn, &a);
  assert(buf != NULL);
  free(buf);
  mizu_arrow_close(a);

  bi = 0;   /* a NULL sizes array: the producer's word */
  memcpy(views + 8, &bi, 4);
  vbufs[3] = NULL;
  buf = frame_write(&root, batches, 1, 0, &total, &warn, &a);
  free(buf);
  mizu_arrow_close(a);

  /* the no-sizes layout proper: one fewer buffer, the last a data slot */
  const void *nbuf3[3] = { NULL, views, data };
  bi = 0;
  memcpy(views + 8, &bi, 4);
  mk_array(&av, 1, 0, 0, 3, nbuf3, NULL, NULL);
  buf = frame_write(&root, batches, 1, 0, &total, &warn, &a);
  assert(buf != NULL);
  assert(mizu_mizl_elem(buf, total, 0, &e) == 0);
  offs = (const int64_t *) (buf + e.data_offset + g.offsets);
  assert(offs[0] == 0 && offs[1] == 20);
  assert(memcmp(buf + e.data_offset + g.data, data, sizeof data) == 0);
  free(buf);
  mizu_arrow_close(a);
}

static void test_str_offsets_malformed(void) {
  /* the null-free span vets every intermediate offset, not just the
     endpoints (C-13): a non-monotonic interior would size one way and
     gather another */
  ArrowSchema cs, root;
  ArrowSchema *kids[1] = { &cs };
  mk_schema(&cs, "u", "s", NULL, 0, NULL, 0);
  mk_schema(&root, "+s", NULL, kids, 1, NULL, 0);
  const char sb[8] = "abcdefg";
  const int32_t so[4] = { 0, 5, 2, 7 };   /* endpoints pass, the interior not */
  const void *sbv[3] = { NULL, so, sb };
  ArrowArray as, batch;
  ArrowArray *bk[1] = { &as };
  mk_array(&as, 3, 0, 0, 3, sbv, NULL, NULL);
  mk_array(&batch, 3, 0, 0, 1, NULL, bk, NULL);
  batch.n_children = 1;
  const ArrowArray *batches[1] = { &batch };
  mizu_arrow *a = NULL;
  assert(mizu_arrow_open(&a, &root, batches, 1, 0) == MIZU_ERR);
  assert(strstr(mizu_last_error_message(), "invalid Arrow string") != NULL);

  /* large_utf8: a per-element span past the int32 element bound */
  const int64_t lo[4] = { 0, (int64_t) INT32_MAX + 2,
                          (int64_t) INT32_MAX + 2, (int64_t) INT32_MAX + 2 };
  const void *lbv[3] = { NULL, lo, sb };
  mk_array(&as, 3, 0, 0, 3, lbv, NULL, NULL);
  mk_schema(&cs, "U", "s", NULL, 0, NULL, 0);
  assert(mizu_arrow_open(&a, &root, batches, 1, 0) == MIZU_ERR);
  assert(strstr(mizu_last_error_message(), "invalid Arrow string") != NULL);

  /* a second batch carrying a null flips the write to the gather: batch
     0's malformed fast-path record declines all the same */
  static char data0[8000];
  memset(data0, 'q', sizeof data0);
  const int32_t off0[4] = { 0, 4000, 2, 8000 };
  const void *b0v[3] = { NULL, off0, data0 };
  const uint8_t bm1 = 0x00;
  const int32_t off1[2] = { 0, 0 };
  const char data1[1] = { 0 };
  const void *b1v[3] = { &bm1, off1, data1 };
  ArrowArray a0, a1, bt0, bt1;
  ArrowArray *k0[1] = { &a0 };
  ArrowArray *k1[1] = { &a1 };
  mk_array(&a0, 3, 0, 0, 3, b0v, NULL, NULL);
  mk_array(&a1, 1, 1, 0, 3, b1v, NULL, NULL);
  mk_array(&bt0, 3, 0, 0, 1, NULL, k0, NULL);
  bt0.n_children = 1;
  mk_array(&bt1, 1, 0, 0, 1, NULL, k1, NULL);
  bt1.n_children = 1;
  const ArrowArray *two[2] = { &bt0, &bt1 };
  mk_schema(&cs, "u", "s", NULL, 0, NULL, 0);
  assert(mizu_arrow_open(&a, &root, two, 2, 0) == MIZU_ERR);
  assert(strstr(mizu_last_error_message(), "invalid Arrow string") != NULL);

  /* the same shape, offsets monotonic: the fast path sizes and the
     rebase writes exactly the packed span */
  const int32_t ok[4] = { 0, 2, 5, 7 };
  const void *okv[3] = { NULL, ok, sb };
  mk_array(&as, 3, 0, 0, 3, okv, NULL, NULL);
  size_t total;
  mizu_arrow_warn warn;
  uint8_t *buf = frame_write(&root, batches, 1, 0, &total, &warn, &a);
  mizu_mizl_entry e;
  assert(mizu_mizl_elem(buf, total, 0, &e) == 0);
  mizu_mizs_geom g = mizu_mizs_geometry(3);
  const int64_t *offs = (const int64_t *) (buf + e.data_offset + g.offsets);
  assert(offs[0] == 0 && offs[1] == 2 && offs[2] == 5 && offs[3] == 7);
  assert(memcmp(buf + e.data_offset + g.data, "abcdefg", 7) == 0);
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

  /* an ordered dictionary: a cap-less peer declines (metadata is data,
     never a strip); a peer advertising the bit opens, the blob carrying
     the two-element class */
  mk_schema(&cd, "c", "d", NULL, 0, &lv, MIZU_ARROW_FLAG_DICTIONARY_ORDERED);
  assert(mizu_arrow_open(&a, &root, batches, 1, 0) == MIZU_ERR);
  assert(strstr(mizu_last_error_message(), "ordered factor capability") !=
         NULL);
  /* a clean ordered frame writes the two-element class */
  ArrowArray cdict, ca, cb2;
  ArrowArray *ck[1] = { &ca };
  const void *cdb[3] = { NULL, levoffs, levbytes };
  const void *ccodes[2] = { NULL, codes };
  mk_array(&cdict, 2, 0, 0, 3, cdb, NULL, NULL);
  mk_array(&ca, 2, 0, 0, 2, ccodes, NULL, &cdict);
  mk_array(&cb2, 2, 0, 0, 1, NULL, ck, NULL);
  cb2.n_children = 1;
  const ArrowArray *cbatches[1] = { &cb2 };
  size_t ototal;
  mizu_arrow_warn owarn;
  uint8_t *obuf = frame_write(&root, cbatches, 1, MIZU_CAP_ORDERED, &ototal,
                              &owarn, &a);
  mizu_mizl_entry oe;
  assert(mizu_mizl_elem(obuf, ototal, 0, &oe) == 0);
  const uint8_t *oblob = obuf + oe.data_offset + oe.data_size - oe.attrs_size;
  assert(find_span(oblob, (size_t) oe.attrs_size, "ordered", 7) != NULL);
  assert(find_span(oblob, (size_t) oe.attrs_size, "factor", 6) != NULL);
  blob_check(oblob, (size_t) oe.attrs_size);
  free(obuf);
  mizu_arrow_close(a);
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

static void test_dict_batch_shape(void) {
  /* every batch's dictionary gets the buffer-shape vet (C-14): a later
     batch's malformed dictionary would otherwise be read unvetted by
     the level match */
  ArrowSchema lv, cd, root;
  ArrowSchema *kids[1] = { &cd };
  mk_schema(&lv, "u", NULL, NULL, 0, NULL, 0);
  mk_schema(&cd, "c", "d", NULL, 0, &lv, 0);
  mk_schema(&root, "+s", NULL, kids, 1, NULL, 0);
  const char levbytes[2] = { 'a', 'b' };
  const int32_t levoffs[3] = { 0, 1, 2 };
  const void *db[3] = { NULL, levoffs, levbytes };
  const int8_t codes[2] = { 0, 1 };
  const void *cb[2] = { NULL, codes };
  ArrowArray dict0, dict1, a0, a1, b0, b1;
  ArrowArray *k0[1] = { &a0 };
  ArrowArray *k1[1] = { &a1 };
  mk_array(&dict0, 2, 0, 0, 3, db, NULL, NULL);
  mk_array(&a0, 2, 0, 0, 2, cb, NULL, &dict0);
  mk_array(&a1, 2, 0, 0, 2, cb, NULL, &dict1);
  mk_array(&b0, 2, 0, 0, 1, NULL, k0, NULL);
  b0.n_children = 1;
  mk_array(&b1, 2, 0, 0, 1, NULL, k1, NULL);
  b1.n_children = 1;
  const ArrowArray *batches[2] = { &b0, &b1 };
  mizu_arrow *a = NULL;

  /* NULL buffers */
  mk_array(&dict1, 2, 0, 0, 3, NULL, NULL, NULL);
  assert(mizu_arrow_open(&a, &root, batches, 2, 0) == MIZU_ERR);
  assert(strstr(mizu_last_error_message(),
                "invalid Arrow C Data Interface") != NULL);

  /* a short buffer count */
  mk_array(&dict1, 2, 0, 0, 2, db, NULL, NULL);
  assert(mizu_arrow_open(&a, &root, batches, 2, 0) == MIZU_ERR);
  assert(strstr(mizu_last_error_message(),
                "invalid Arrow C Data Interface") != NULL);

  /* a NULL offsets span with levels present */
  const void *dbn[3] = { NULL, NULL, levbytes };
  mk_array(&dict1, 2, 0, 0, 3, dbn, NULL, NULL);
  assert(mizu_arrow_open(&a, &root, batches, 2, 0) == MIZU_ERR);
  assert(strstr(mizu_last_error_message(),
                "invalid Arrow C Data Interface") != NULL);

  /* no dictionary at all */
  mk_array(&a1, 2, 0, 0, 2, cb, NULL, NULL);
  assert(mizu_arrow_open(&a, &root, batches, 2, 0) == MIZU_ERR);
  assert(strstr(mizu_last_error_message(),
                "invalid Arrow C Data Interface") != NULL);

  /* the same dictionary on both batches: opens, the codes gather */
  mk_array(&a1, 2, 0, 0, 2, cb, NULL, &dict1);
  mk_array(&dict1, 2, 0, 0, 3, db, NULL, NULL);
  size_t total;
  mizu_arrow_warn warn;
  uint8_t *buf = frame_write(&root, batches, 2, 0, &total, &warn, &a);
  mizu_mizl_entry e;
  assert(mizu_mizl_elem(buf, total, 0, &e) == 0);
  const int32_t *got = (const int32_t *) (buf + e.data_offset);
  assert(got[0] == 1 && got[1] == 2 && got[2] == 1 && got[3] == 2);
  free(buf);
  mizu_arrow_close(a);
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

static void test_uint64_verbatim(void) {
  /* 'L': the verbatim INT64 leaf + the u64 blob attr with a capable peer
     word, the REAL conversion without; the n_int64 count and its
     exclusions */
  ArrowSchema cl, ci, root;
  ArrowSchema *kids[2] = { &cl, &ci };
  mk_schema(&cl, "L", "u", NULL, 0, NULL, 0);
  mk_schema(&ci, "l", "i", NULL, 0, NULL, 0);
  mk_schema(&root, "+s", NULL, kids, 2, NULL, 0);

  /* element 1 null in both; u's element 2 an INT64_MIN pattern (2^63
     unsigned), i's element 0 a genuine INT64_MIN marked valid */
  const uint8_t bm = 0x05;
  const uint64_t uv[3] = { 42, 0, 0x8000000000000000ULL };
  const int64_t iv[3] = { INT64_MIN, 0, 7 };
  const void *ub[2] = { &bm, uv };
  const void *ib[2] = { &bm, iv };
  ArrowArray au, ai, batch;
  ArrowArray *bk[2] = { &au, &ai };
  mk_array(&au, 3, 1, 0, 2, ub, NULL, NULL);
  mk_array(&ai, 3, 1, 0, 2, ib, NULL, NULL);
  mk_array(&batch, 3, 0, 0, 1, NULL, bk, NULL);
  batch.n_children = 2;
  const ArrowArray *batches[1] = { &batch };
  size_t total;
  mizu_arrow_warn warn;
  mizu_arrow *a;
  mizu_mizl_entry e;

  /* the capable word: the verbatim INT64 leaf, the u64 pair present; the
     u64 column's INT64_MIN-pattern lane does not count */
  uint8_t *buf = frame_write(&root, batches, 1, MIZU_CAP_MIZL_U64V, &total,
                             &warn, &a);
  assert(warn.n_int64 == 1 && warn.n_range == 0 && warn.n_intmin == 0);
  assert(mizu_mizl_elem(buf, total, 0, &e) == 0);
  assert(e.sexptype == MIZU_TYPE_INT64 && e.attrs_size > 0);
  const int64_t *v = (const int64_t *) (buf + e.data_offset);
  assert(v[0] == 42 && v[1] == MIZU_NA_INT64);
  uint64_t bits;
  memcpy(&bits, &v[2], 8);
  assert(bits == 0x8000000000000000ULL);   /* verbatim */
  const uint8_t *blob = buf + e.data_offset + e.data_size - e.attrs_size;
  assert(find_span(blob, (size_t) e.attrs_size, "u64", 3) != NULL);
  blob_check(blob, (size_t) e.attrs_size);
  assert(mizu_mizl_elem(buf, total, 1, &e) == 0);
  assert(e.sexptype == MIZU_TYPE_INT64 && e.attrs_size == 0);
  free(buf);
  mizu_arrow_close(a);

  /* a peer word short of the bit: the integer64 conversion, the attr
     absent */
  buf = frame_write(&root, batches, 1, 0, &total, &warn, &a);
  assert(warn.n_range == 1 && warn.n_int64 == 1 && warn.n_intmin == 0);
  assert(mizu_mizl_elem(buf, total, 0, &e) == 0);
  assert(e.sexptype == MIZU_TYPE_INT64 && e.attrs_size == 0);
  const int64_t *d = (const int64_t *) (buf + e.data_offset);
  assert(d[0] == 42);
  memcpy(&bits, &d[2], 8);
  assert(bits == 0x8000000000000000ULL);   /* 2^63 past INT64_MAX: the NA
                                              sentinel's bits */
  free(buf);
  mizu_arrow_close(a);

  /* the unmasked path counts too: a null-free column's genuine sentinel
     warns exactly like a masked one's */
  {
    ArrowSchema ci1, root1;
    ArrowSchema *kids1[1] = { &ci1 };
    mk_schema(&ci1, "l", "i", NULL, 0, NULL, 0);
    mk_schema(&root1, "+s", NULL, kids1, 1, NULL, 0);
    const int64_t xv[2] = { INT64_MIN, 5 };
    const void *xb[2] = { NULL, xv };
    ArrowArray ax, bx;
    ArrowArray *kx[1] = { &ax };
    mk_array(&ax, 2, 0, 0, 2, xb, NULL, NULL);
    mk_array(&bx, 2, 0, 0, 1, NULL, kx, NULL);
    bx.n_children = 1;
    const ArrowArray *b1[1] = { &bx };
    buf = frame_write(&root1, b1, 1, 0, &total, &warn, &a);
    assert(warn.n_int64 == 1);
    free(buf);
    mizu_arrow_close(a);
  }
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

static void test_short_formats(void) {
  /* short format strings decline without a read past the NUL (C-16);
     the well-formed temporal forms are test_temporal's */
  ArrowSchema cx, root;
  ArrowSchema *kids[1] = { &cx };
  mk_schema(&root, "+s", NULL, kids, 1, NULL, 0);
  const int64_t v[1] = { 0 };
  const void *xb[2] = { NULL, v };
  ArrowArray ax, batch;
  ArrowArray *bk[1] = { &ax };
  mk_array(&ax, 1, 0, 0, 2, xb, NULL, NULL);
  mk_array(&batch, 1, 0, 0, 1, NULL, bk, NULL);
  batch.n_children = 1;
  const ArrowArray *batches[1] = { &batch };
  mizu_arrow *a = NULL;
  static const char *const fmts[] = { "", "t", "ts", "tD", "tds",
                                      "tsx", "tsu" };
  for (size_t i = 0; i < sizeof fmts / sizeof fmts[0]; i++) {
    mk_schema(&cx, fmts[i], "x", NULL, 0, NULL, 0);
    assert(mizu_arrow_open(&a, &root, batches, 1, 0) == MIZU_ERR);
    assert(strstr(mizu_last_error_message(), "no portable home") != NULL);
  }
}

static void test_struct_batch_shape(void) {
  /* a batch's own offset or struct-null bitmap declines (C-19): the
     writer stages the children's slot values as rows, so a batch
     carrying either would mis-frame */
  ArrowSchema cx, root;
  ArrowSchema *kids[1] = { &cx };
  mk_schema(&cx, "i", "x", NULL, 0, NULL, 0);
  mk_schema(&root, "+s", NULL, kids, 1, NULL, 0);
  const int32_t x[3] = { 7, 8, 9 };
  const void *xb[2] = { NULL, x };
  ArrowArray ax, batch;
  ArrowArray *bk[1] = { &ax };
  mk_array(&ax, 3, 0, 0, 2, xb, NULL, NULL);
  const ArrowArray *batches[1] = { &batch };
  mizu_arrow *a = NULL;

  /* a struct offset */
  mk_array(&batch, 3, 0, 1, 1, NULL, bk, NULL);
  batch.n_children = 1;
  assert(mizu_arrow_open(&a, &root, batches, 1, 0) == MIZU_ERR);
  assert(strstr(mizu_last_error_message(), "struct offset") != NULL);

  /* struct nulls with a bitmap */
  const uint8_t sbm = 0x05;
  const void *sbufs[1] = { &sbm };
  mk_array(&batch, 3, 1, 0, 1, sbufs, bk, NULL);
  batch.n_children = 1;
  assert(mizu_arrow_open(&a, &root, batches, 1, 0) == MIZU_ERR);
  assert(strstr(mizu_last_error_message(), "null struct rows") != NULL);

  /* a NULL child ArrowArray pointer declines, never dereferences */
  ArrowArray *bkn[1] = { NULL };
  mk_array(&batch, 3, 0, 0, 1, NULL, bkn, NULL);
  batch.n_children = 1;
  assert(mizu_arrow_open(&a, &root, batches, 1, 0) == MIZU_ERR);
  assert(strstr(mizu_last_error_message(),
                "invalid Arrow C Data Interface") != NULL);

  /* null_count -1 with no bitmap: the slot values are the rows */
  mk_array(&batch, 3, -1, 0, 1, NULL, bk, NULL);
  batch.n_children = 1;
  size_t total;
  mizu_arrow_warn warn;
  uint8_t *buf = frame_write(&root, batches, 1, 0, &total, &warn, &a);
  mizu_mizl_entry e;
  assert(mizu_mizl_elem(buf, total, 0, &e) == 0);
  const int32_t *v = (const int32_t *) (buf + e.data_offset);
  assert(v[0] == 7 && v[1] == 8 && v[2] == 9);
  free(buf);
  mizu_arrow_close(a);
}

/* The conversion kernels, direct: the f16 edge values, the rows no
   single-char format maps to (numpy's '?' bool8 and 'c8' complex), the
   all-null lane fill, and a 64-lane validity word straddling a ninth
   byte. */
static void test_cvt_kernels(void) {
  mizu_arrow_warn warn;
  memset(&warn, 0, sizeof warn);

  /* f16: inf/nan, ±0, and the subnormal normalization loop */
  const uint16_t h[7] = { 0x7C00, 0xFC00, 0x7E00, 0x0000, 0x8000, 0x0001,
                          0x03FF };
  double d[7];
  mizu_cvt_convert((uint8_t *) d, (const uint8_t *) h, NULL, 0, 7,
                   mizu_cvt_for_arrow("e"), &warn);
  assert(d[0] == 1.0 / 0.0 && d[1] == -1.0 / 0.0);
  assert(d[2] != d[2]);
  assert(d[3] == 0.0 && d[4] == 0.0);
  assert(d[5] == 5.9604644775390625e-08);        /* 2^-24 */
  assert(d[6] == 1023.0 * 5.9604644775390625e-08);

  /* bool8: one byte per source lane */
  const mizu_cvt_row b8 = { MIZU_TYPE_LGL, MIZU_CVT_BOOL8_LGL, 1, 4 };
  const uint8_t bs[4] = { 0, 1, 0, 7 };
  int32_t bo[4];
  mizu_cvt_convert((uint8_t *) bo, bs, NULL, 0, 4, &b8, &warn);
  assert(bo[0] == 0 && bo[1] == 1 && bo[2] == 0 && bo[3] == 1);

  /* c64: float re/im pairs -> double re/im */
  const mizu_cvt_row c64 = { MIZU_TYPE_CPLX, MIZU_CVT_C64_CPLX, 8, 16 };
  const float cs[4] = { 1.5f, -2.5f, 0.25f, 4.0f };
  double co[4];
  mizu_cvt_convert((uint8_t *) co, (const uint8_t *) cs, NULL, 0, 2, &c64,
                   &warn);
  assert(co[0] == 1.5 && co[1] == -2.5 && co[2] == 0.25 && co[3] == 4.0);

  /* masked lanes: an all-null group fills the sentinel whole */
  const mizu_cvt_row *i32 = mizu_cvt_for_arrow("i");
  int32_t src[65];
  for (int i = 0; i < 65; i++) src[i] = i;
  int32_t dst[65];
  const uint8_t v0[9] = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };
  mizu_cvt_convert((uint8_t *) dst, (const uint8_t *) src, v0, 0, 64, i32,
                   &warn);
  assert(dst[0] == MIZU_NA_INT32 && dst[63] == MIZU_NA_INT32);

  /* bit offset 1 over 64 lanes: the validity word straddles a ninth
     byte — lane 63 rides bit 0 of byte 8 */
  uint8_t v1[10];
  memset(v1, 0xFF, sizeof v1);
  v1[0] = 0xFD;   /* lane 0 (bit 1) null */
  v1[8] = 0xFE;   /* lane 63 (byte 8 bit 0) null */
  mizu_cvt_convert((uint8_t *) dst, (const uint8_t *) src, v1, 1, 65, i32,
                   &warn);
  assert(dst[0] == MIZU_NA_INT32 && dst[1] == 1 && dst[62] == 62);
  assert(dst[63] == MIZU_NA_INT32 && dst[64] == 64);

  /* the uint64 row: exact through INT64_MAX, the sentinel and the count
     past it */
  const mizu_cvt_row *u64 = mizu_cvt_for_arrow("L");
  const uint64_t us[5] = { 0, 9007199254740993ULL, 9223372036854775807ULL,
                           9223372036854775808ULL, 18446744073709551615ULL };
  int64_t uo[5];
  mizu_cvt_convert((uint8_t *) uo, (const uint8_t *) us, NULL, 0, 5, u64,
                   &warn);
  assert(warn.n_range == 2);
  assert(uo[0] == 0 && uo[1] == 9007199254740993LL &&
         uo[2] == 9223372036854775807LL);
  assert(uo[3] == MIZU_NA_INT64 && uo[4] == MIZU_NA_INT64);

  /* the format table: unknown and multi-char formats miss; 'L' resolves
     (classify's special case never consults the table for it) */
  assert(mizu_cvt_for_arrow("") == NULL);
  assert(mizu_cvt_for_arrow("xx") == NULL);
  assert(mizu_cvt_for_arrow("?") == NULL);
  assert(mizu_cvt_for_arrow("L") != NULL);
  assert(mizu_cvt_for_arrow("L")->wire == MIZU_TYPE_INT64);
}

/* Open-time declines the frame tests have no shape for. */
static void test_open_declines2(void) {
  ArrowSchema cx, lv, root;
  ArrowSchema *kids[1] = { &cx };
  mizu_arrow *a = NULL;

  /* the root schema is not a struct */
  mk_schema(&root, "i", NULL, kids, 1, NULL, 0);
  assert(mizu_arrow_open(&a, &root, NULL, 0, 0) == MIZU_ERR);
  assert(strstr(mizu_last_error_message(), "not a struct") != NULL);

  mk_schema(&root, "+s", NULL, kids, 1, NULL, 0);
  const int32_t x[2] = { 1, 2 };
  const void *xb[2] = { NULL, x };
  ArrowArray ax, batch;
  ArrowArray *bk[1] = { &ax };
  mk_array(&ax, 2, 0, 0, 2, xb, NULL, NULL);
  mk_array(&batch, 2, 0, 0, 1, NULL, bk, NULL);
  batch.n_children = 1;
  const ArrowArray *batches[1] = { &batch };

  /* a column with no format */
  mk_schema(&cx, NULL, "x", NULL, 0, NULL, 0);
  assert(mizu_arrow_open(&a, &root, batches, 1, 0) == MIZU_ERR);
  assert(strstr(mizu_last_error_message(), "no format") != NULL);

  /* a dictionary with a non-integer index */
  mk_schema(&cx, "f", "d", NULL, 0, &lv, 0);
  mk_schema(&lv, "u", NULL, NULL, 0, NULL, 0);
  assert(mizu_arrow_open(&a, &root, batches, 1, 0) == MIZU_ERR);
  assert(strstr(mizu_last_error_message(), "non-integer index") != NULL);

  /* a timezone name past the plan's storage */
  char tzfmt[80];
  memset(tzfmt, 'x', 4 + 70);
  memcpy(tzfmt, "tsu:", 4);
  tzfmt[4 + 70] = '\0';
  mk_schema(&cx, tzfmt, "t", NULL, 0, NULL, 0);
  assert(mizu_arrow_open(&a, &root, batches, 1, 0) == MIZU_ERR);
  assert(strstr(mizu_last_error_message(), "timezone name") != NULL);

  /* batch shape: a string column short a buffer, then with a NULL data
     buffer */
  mk_schema(&cx, "u", "s", NULL, 0, NULL, 0);
  ArrowArray as, sbatch;
  ArrowArray *sk[1] = { &as };
  const int32_t so[3] = { 0, 1, 2 };
  const void *short_bufs[2] = { NULL, so };
  mk_array(&as, 2, 0, 0, 2, short_bufs, NULL, NULL);
  mk_array(&sbatch, 2, 0, 0, 1, NULL, sk, NULL);
  sbatch.n_children = 1;
  const ArrowArray *sb1[1] = { &sbatch };
  assert(mizu_arrow_open(&a, &root, sb1, 1, 0) == MIZU_ERR);
  assert(strstr(mizu_last_error_message(), "invalid Arrow C Data") !=
         NULL);
  const void *null_data[3] = { NULL, so, NULL };
  mk_array(&as, 2, 0, 0, 3, null_data, NULL, NULL);
  assert(mizu_arrow_open(&a, &root, sb1, 1, 0) == MIZU_ERR);
  assert(strstr(mizu_last_error_message(), "invalid Arrow C Data") !=
         NULL);

  /* a dictionary whose level offsets walk backwards: the levels walk
     fails its vet */
  ArrowSchema cd2, lv2;
  ArrowSchema *dk[1] = { &cd2 };
  mk_schema(&lv2, "u", NULL, NULL, 0, NULL, 0);
  mk_schema(&cd2, "c", "d", NULL, 0, &lv2, 0);
  mk_schema(&root, "+s", NULL, dk, 1, NULL, 0);
  const char levbytes[7] = { 'l', 'o', 'w', 'h', 'i', 'g', 'h' };
  const int32_t badoffs[3] = { 0, 5, 2 };
  ArrowArray dict, ai, dbatch;
  ArrowArray *dkids[1] = { &ai };
  const void *db[3] = { NULL, badoffs, levbytes };
  mk_array(&dict, 2, 0, 0, 3, db, NULL, NULL);
  const int8_t codes[2] = { 0, 1 };
  const void *cb[2] = { NULL, codes };
  mk_array(&ai, 2, 0, 0, 2, cb, NULL, &dict);
  mk_array(&dbatch, 2, 0, 0, 1, NULL, dkids, NULL);
  dbatch.n_children = 1;
  const ArrowArray *db1[1] = { &dbatch };
  assert(mizu_arrow_open(&a, &root, db1, 1, 0) == MIZU_ERR);
  assert(strstr(mizu_last_error_message(),
                "invalid Arrow dictionary export") != NULL);
}

/* Dictionary index widths: every integer width signs and stages. */
static void test_rows_bound(void) {
  /* the automatic row.names are the compact c(NA, -n) int32 form: past
     2^31 rows the open declines instead of wrapping the emit */
  ArrowSchema ci, root;
  ArrowSchema *kids[1] = { &ci };
  mk_schema(&ci, "i", "x", NULL, 0, NULL, 0);
  mk_schema(&root, "+s", NULL, kids, 1, NULL, 0);
  const int32_t x[1] = { 1 };
  const void *xb[2] = { NULL, x };
  ArrowArray ax, ax1, b1, b2;
  ArrowArray *bk[1] = { &ax };
  ArrowArray *bk1[1] = { &ax1 };
  mk_array(&ax, (int64_t) INT32_MAX + 1, 0, 0, 2, xb, NULL, NULL);
  mk_array(&b1, (int64_t) INT32_MAX + 1, 0, 0, 1, NULL, bk, NULL);
  b1.n_children = 1;
  const ArrowArray *batches[1] = { &b1 };
  mizu_arrow *a = NULL;
  assert(mizu_arrow_open(&a, &root, batches, 1, 0) == MIZU_ERR);
  assert(strstr(mizu_last_error_message(), "int32 row range") != NULL);

  /* the sum across batches is bounded too */
  mk_array(&ax, INT32_MAX, 0, 0, 2, xb, NULL, NULL);
  mk_array(&b1, INT32_MAX, 0, 0, 1, NULL, bk, NULL);
  b1.n_children = 1;
  mk_array(&ax1, 1, 0, 0, 2, xb, NULL, NULL);
  mk_array(&b2, 1, 0, 0, 1, NULL, bk1, NULL);
  b2.n_children = 1;
  const ArrowArray *batches2[2] = { &b1, &b2 };
  assert(mizu_arrow_open(&a, &root, batches2, 2, 0) == MIZU_ERR);

  /* the boundary itself opens (the 8 GiB write stays untested here) */
  const ArrowArray *batches3[1] = { &b1 };
  assert(mizu_arrow_open(&a, &root, batches3, 1, 0) == MIZU_OK);
  assert(mizu_arrow_rows(a) == INT32_MAX);
  mizu_arrow_close(a);
}

static void test_dict_widths(void) {
  ArrowSchema lv, cd, root;
  ArrowSchema *kids[1] = { &cd };
  mk_schema(&lv, "u", NULL, NULL, 0, NULL, 0);
  mk_schema(&cd, "c", "d", NULL, 0, &lv, 0);
  mk_schema(&root, "+s", NULL, kids, 1, NULL, 0);
  const char levbytes[7] = { 'l', 'o', 'w', 'h', 'i', 'g', 'h' };
  const int32_t levoffs[3] = { 0, 3, 7 };
  ArrowArray dict;
  const void *db[3] = { NULL, levoffs, levbytes };
  mk_array(&dict, 2, 0, 0, 3, db, NULL, NULL);

  const uint8_t cC[3] = { 0, 1, 0 };
  const uint16_t cS[3] = { 0, 1, 0 };
  const int32_t ci[3] = { 0, 1, 0 };
  const uint32_t cI[3] = { 0, 1, 0 };
  const int64_t cl[3] = { 0, 1, 0 };
  const uint64_t cL[3] = { 0, 1, 0 };
  const char fmts[6][2] = { "C", "S", "i", "I", "l", "L" };
  const void *codes[6] = { cC, cS, ci, cI, cl, cL };
  const uint8_t nul = 0x05;   /* slot 1: the NA'd code */
  for (int k = 0; k < 6; k++) {
    mk_schema(&cd, fmts[k], "d", NULL, 0, &lv, 0);
    const void *cb[2] = { &nul, codes[k] };
    ArrowArray ai, batch;
    ArrowArray *bk[1] = { &ai };
    mk_array(&ai, 3, 1, 0, 2, cb, NULL, &dict);
    mk_array(&batch, 3, 0, 0, 1, NULL, bk, NULL);
    batch.n_children = 1;
    const ArrowArray *batches[1] = { &batch };
    size_t total;
    mizu_arrow_warn warn;
    mizu_arrow *a;
    uint8_t *buf = frame_write(&root, batches, 1, 0, &total, &warn, &a);
    mizu_mizl_entry e;
    assert(mizu_mizl_elem(buf, total, 0, &e) == 0);
    assert(e.sexptype == MIZU_TYPE_INT && e.length == 3);
    const int32_t *v = (const int32_t *) (buf + e.data_offset);
    assert(v[0] == 1 && v[1] == MIZU_NA_INT32 && v[2] == 1);
    free(buf);
    mizu_arrow_close(a);
  }
}

/* A dict column on a zero-batch stream stages as the empty factor; a
   one-level dictionary writes the scalar str form. */
static void test_dict_edge_shapes(void) {
  ArrowSchema lv, cd, root;
  ArrowSchema *kids[1] = { &cd };
  mk_schema(&lv, "u", NULL, NULL, 0, NULL, 0);
  mk_schema(&cd, "c", "d", NULL, 0, &lv, 0);
  mk_schema(&root, "+s", NULL, kids, 1, NULL, 0);
  mizu_arrow_warn warn;

  /* zero batches: the canonical levels are empty, the codes body empty */
  size_t total;
  mizu_arrow *a;
  uint8_t *buf = frame_write(&root, NULL, 0, 0, &total, &warn, &a);
  int64_t aoff, asz, valid[2];
  assert(frame_check(buf, total, &aoff, &asz, valid) == 1);
  mizu_mizl_entry e;
  assert(mizu_mizl_elem(buf, total, 0, &e) == 0);
  assert(e.length == 0);
  blob_check(buf + e.data_offset + e.data_size - e.attrs_size,
             (size_t) e.attrs_size);
  free(buf);
  mizu_arrow_close(a);

  /* one level: the blob's levels value is a bare str, not a strv */
  const char levbytes[3] = { 'l', 'o', 'w' };
  const int32_t levoffs[2] = { 0, 3 };
  ArrowArray dict;
  const void *db[3] = { NULL, levoffs, levbytes };
  mk_array(&dict, 1, 0, 0, 3, db, NULL, NULL);
  const int8_t codes[2] = { 0, 0 };
  const void *cb[2] = { NULL, codes };
  ArrowArray ai, batch;
  ArrowArray *bk[1] = { &ai };
  mk_array(&ai, 2, 0, 0, 2, cb, NULL, &dict);
  mk_array(&batch, 2, 0, 0, 1, NULL, bk, NULL);
  batch.n_children = 1;
  const ArrowArray *batches[1] = { &batch };
  buf = frame_write(&root, batches, 1, 0, &total, &warn, &a);
  assert(mizu_mizl_elem(buf, total, 0, &e) == 0);
  const int32_t *v = (const int32_t *) (buf + e.data_offset);
  assert(v[0] == 1 && v[1] == 1);   /* codes are 0-based + 1 */
  const uint8_t *blob = buf + e.data_offset + e.data_size - e.attrs_size;
  assert(find_span(blob, (size_t) e.attrs_size, "\x04\x03\x00\x00\x00low",
                   8) != NULL);   /* the str form, not a strv */
  blob_check(blob, (size_t) e.attrs_size);
  free(buf);
  mizu_arrow_close(a);
}

static void test_dict_strview_inline(void) {
  /* a string_view dictionary whose levels are all inline (<= 12 bytes)
     legally carries no variadic data buffer (n_buffers == 2) */
  ArrowSchema lv, cd, root;
  ArrowSchema *kids[1] = { &cd };
  mk_schema(&lv, "vu", NULL, NULL, 0, NULL, 0);
  mk_schema(&cd, "c", "d", NULL, 0, &lv, 0);
  mk_schema(&root, "+s", NULL, kids, 1, NULL, 0);
  uint8_t views[32];
  memset(views, 0, sizeof views);
  int32_t l0 = 3, l1 = 4;
  memcpy(views, &l0, 4);
  memcpy(views + 4, "low", 3);
  memcpy(views + 16, &l1, 4);
  memcpy(views + 20, "high", 4);
  const void *db[2] = { NULL, views };
  ArrowArray dict, ai, batch;
  mk_array(&dict, 2, 0, 0, 2, db, NULL, NULL);
  const int8_t codes[4] = { 0, 1, 1, 0 };
  const void *cb[2] = { NULL, codes };
  ArrowArray *bk[1] = { &ai };
  mk_array(&ai, 4, 0, 0, 2, cb, NULL, &dict);
  mk_array(&batch, 4, 0, 0, 1, NULL, bk, NULL);
  batch.n_children = 1;
  const ArrowArray *batches[1] = { &batch };
  size_t total;
  mizu_arrow_warn warn;
  mizu_arrow *a;
  uint8_t *buf = frame_write(&root, batches, 1, 0, &total, &warn, &a);
  mizu_mizl_entry e;
  assert(mizu_mizl_elem(buf, total, 0, &e) == 0);
  const int32_t *v = (const int32_t *) (buf + e.data_offset);
  assert(v[0] == 1 && v[1] == 2 && v[2] == 2 && v[3] == 1);
  const uint8_t *blob = buf + e.data_offset + e.data_size - e.attrs_size;
  assert(find_span(blob, (size_t) e.attrs_size, "low", 3) != NULL);
  assert(find_span(blob, (size_t) e.attrs_size, "high", 4) != NULL);
  blob_check(blob, (size_t) e.attrs_size);
  free(buf);
  mizu_arrow_close(a);

  /* a >12-byte level with no data buffer to name declines at the open's
     levels walk */
  int32_t l2 = 13, bi = 0, bo = 0;
  memcpy(views, &l2, 4);
  memcpy(views + 8, &bi, 4);
  memcpy(views + 12, &bo, 4);
  assert(mizu_arrow_open(&a, &root, batches, 1, 0) == MIZU_ERR);
  assert(strstr(mizu_last_error_message(),
                "invalid Arrow dictionary") != NULL);
}

/* A referenced one-column int32 frame of the given length (defined with
   the remote-leaf tests below). */
static uint8_t *ref_region(int with_nulls, int64_t rows, size_t *total_out);

/* The remote override's arg vetting. */
static void test_remote_decline(void) {
  ArrowSchema cx, root;
  ArrowSchema *kids[1] = { &cx };
  mk_schema(&cx, "i", "x", NULL, 0, NULL, 0);
  mk_schema(&root, "+s", NULL, kids, 1, NULL, 0);
  const int32_t x[4] = { 1, 2, 3, 4 };
  const void *xb[2] = { NULL, x };
  ArrowArray ax, batch;
  ArrowArray *bk[1] = { &ax };
  mk_array(&ax, 4, 0, 0, 2, xb, NULL, NULL);
  mk_array(&batch, 4, 0, 0, 1, NULL, bk, NULL);
  batch.n_children = 1;
  const ArrowArray *batches[1] = { &batch };
  mizu_arrow *a = NULL;
  assert(mizu_arrow_open(&a, &root, batches, 1, 0) == MIZU_OK);

  /* a referenced frame of the wrong length: the override declines, the
     column stays local */
  size_t reftotal;
  uint8_t *ref = ref_region(0, 5, &reftotal);   /* the frame has 4 rows */
  const char *ident = "/mizu_testref";
  assert(mizu_arrow_remote(a, 0, ident, strlen(ident), ref, reftotal,
                           0) == -1);
  assert(mizu_arrow_remote(a, 9, ident, strlen(ident), ref, reftotal,
                           0) == -1);   /* no such column */
  assert(mizu_arrow_remote(a, 0, NULL, 0, ref, reftotal, 0) == -1);
  mizu_arrow_warn warn;
  size_t total = mizu_arrow_size(a);
  uint8_t *buf = malloc(total);
  assert(buf != NULL);
  memset(&warn, 0, sizeof warn);
  mizu_arrow_write(a, buf, &warn);
  mizu_mizl_entry e;
  assert(mizu_mizl_elem(buf, total, 0, &e) == 0);
  assert(e.sexptype == MIZU_TYPE_INT);   /* still the local leaf */
  free(buf);
  free(ref);
  mizu_arrow_close(a);
}

// The remote leaf --------------------------------------------------------------------

/* A referenced one-column int32 frame of the given length; nulls per the
   argument. */
static uint8_t *ref_region(int with_nulls, int64_t rows, size_t *total_out) {
  ArrowSchema cx, root;
  ArrowSchema *kids[1] = { &cx };
  mk_schema(&cx, "i", "x", NULL, 0, NULL, 0);
  mk_schema(&root, "+s", NULL, kids, 1, NULL, 0);
  const uint8_t bm = 0x05;
  int32_t x[128];
  assert(rows <= 128);
  for (int64_t i = 0; i < rows; i++) x[i] = 7 + (int32_t) i;
  const void *xb[2] = { with_nulls ? &bm : NULL, x };
  ArrowArray ax, batch;
  ArrowArray *bk[1] = { &ax };
  mk_array(&ax, rows, with_nulls, 0, 2, xb, NULL, NULL);
  mk_array(&batch, rows, 0, 0, 1, NULL, bk, NULL);
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
  uint8_t *ref = ref_region(0, 3, &reftotal);
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
  uint8_t *ref2 = ref_region(1, 3, &ref2total);
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

  /* a referenced leaf of another length: the override declines, the
     column stays local (C-20) */
  ArrowSchema cz, root2;
  ArrowSchema *kids2[1] = { &cz };
  mk_schema(&cz, "i", "z", NULL, 0, NULL, 0);
  mk_schema(&root2, "+s", NULL, kids2, 1, NULL, 0);
  const int32_t z[4] = { 1, 2, 3, 4 };
  const void *zb[2] = { NULL, z };
  ArrowArray az, bz;
  ArrowArray *kz[1] = { &az };
  mk_array(&az, 4, 0, 0, 2, zb, NULL, NULL);
  mk_array(&bz, 4, 0, 0, 1, NULL, kz, NULL);
  bz.n_children = 1;
  const ArrowArray *bb[1] = { &bz };
  assert(mizu_arrow_open(&a, &root2, bb, 1, 0) == MIZU_OK);
  assert(mizu_arrow_remote(a, 0, ident, strlen(ident), ref, reftotal,
                           0) == -1);
  size_t t4 = mizu_arrow_size(a);
  uint8_t *b4 = calloc(1, t4);
  mizu_arrow_write(a, b4, &warn);
  assert(mizu_mizl_elem(b4, t4, 0, &e) == 0);
  assert(e.sexptype == MIZU_TYPE_INT && e.length == 4);   /* stayed local */
  free(b4);
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

static void test_override_after_size(void) {
  /* an override registered after the size pass re-runs it: the span and
     the layout track the override, not the memo */
  enum { N = 100 };
  ArrowSchema ct, cx, root;
  ArrowSchema *kids[2] = { &ct, &cx };
  mk_schema(&ct, "tsu:", "t", NULL, 0, NULL, 0);
  mk_schema(&cx, "i", "x", NULL, 0, NULL, 0);
  mk_schema(&root, "+s", NULL, kids, 2, NULL, 0);
  int64_t ts[N];
  int32_t x[N];
  for (int i = 0; i < N; i++) { ts[i] = 1700000000 + i; x[i] = i; }
  const void *tb[2] = { NULL, ts };
  const void *xb[2] = { NULL, x };
  ArrowArray at, ax, batch;
  ArrowArray *bk[2] = { &at, &ax };
  mk_array(&at, N, 0, 0, 2, tb, NULL, NULL);
  mk_array(&ax, N, 0, 0, 2, xb, NULL, NULL);
  mk_array(&batch, N, 0, 0, 1, NULL, bk, NULL);
  batch.n_children = 2;
  const ArrowArray *batches[1] = { &batch };
  mizu_arrow_warn warn;
  memset(&warn, 0, sizeof warn);

  /* tz: a 100-char override outgrows the classified "UTC" past any
     alignment quantum */
  char longtz[101];
  memset(longtz, 'Z', 100);
  longtz[100] = 0;
  mizu_arrow *a = NULL;
  assert(mizu_arrow_open(&a, &root, batches, 1, 0) == MIZU_OK);
  size_t before = mizu_arrow_size(a);
  mizu_arrow_tz(a, 0, longtz);
  size_t after = mizu_arrow_size(a);
  assert(after > before);
  uint8_t *buf = calloc(1, after);
  mizu_arrow_write(a, buf, &warn);
  mizu_mizl_entry e;
  assert(mizu_mizl_elem(buf, after, 0, &e) == 0);
  const uint8_t *blob = buf + e.data_offset + e.data_size - e.attrs_size;
  assert(find_span(blob, (size_t) e.attrs_size, longtz, 100) != NULL);
  assert(find_span(blob, (size_t) e.attrs_size, "UTC", 3) == NULL);
  free(buf);
  mizu_arrow_close(a);

  /* root blob: a caller-built form with a long pad value */
  char pad[101];
  memset(pad, 'P', 100);
  pad[100] = 0;
  uint8_t rb[256];
  size_t n = 0;
  n += mizu_ix_put_header(rb + n);
  n += mizu_ix_put_dict_begin(rb + n, 4);
  n += mizu_ix_put_key(rb + n, "names", 5);
  n += mizu_ix_put_strv_begin(rb + n, 2);
  n += mizu_ix_put_strelt(rb + n, "t", 1);
  n += mizu_ix_put_strelt(rb + n, "x", 1);
  n += mizu_ix_put_key(rb + n, "class", 5);
  n += mizu_ix_put_str(rb + n, "tbl", 3);
  n += mizu_ix_put_key(rb + n, "row.names", 9);
  n += mizu_ix_put_str(rb + n, "a", 1);
  n += mizu_ix_put_key(rb + n, "pad", 3);
  n += mizu_ix_put_str(rb + n, pad, 100);
  assert(mizu_arrow_open(&a, &root, batches, 1, 0) == MIZU_OK);
  before = mizu_arrow_size(a);
  mizu_arrow_root_blob(a, rb, n);
  after = mizu_arrow_size(a);
  assert(after > before);
  buf = calloc(1, after);
  mizu_arrow_write(a, buf, &warn);
  int64_t aoff, asz, valid[2];
  assert(frame_check(buf, after, &aoff, &asz, valid) == 2);
  assert(asz == (int64_t) n);
  assert(memcmp(buf + aoff, rb, n) == 0);
  free(buf);
  mizu_arrow_close(a);

  /* remote: the local int32 leaf collapses to the identifier span */
  size_t reftotal;
  uint8_t *ref = ref_region(0, N, &reftotal);
  assert(mizu_arrow_open(&a, &root, batches, 1, 0) == MIZU_OK);
  before = mizu_arrow_size(a);
  const char *ident = "/mizu_testref";
  assert(mizu_arrow_remote(a, 1, ident, strlen(ident), ref, reftotal,
                           0) == 0);
  after = mizu_arrow_size(a);
  assert(after < before);
  buf = calloc(1, after);
  mizu_arrow_write(a, buf, &warn);
  assert(mizu_mizl_elem(buf, after, 1, &e) == 0);
  assert(e.sexptype == 33);
  assert(e.data_size == (int64_t) strlen("/mizu_testref[1]"));
  free(buf);
  mizu_arrow_close(a);
  free(ref);
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

  /* the churn signal declines ahead of the region checkout */
  mizu_arrow_close(stage_arrow);
  assert(mizu_arrow_open(&stage_arrow, &root, batches, 1, caps) ==
         MIZU_OK);
  ((mizu_handle *) host)->fl.churn = 1;
  assert(mizu_channel_send(host, &(mizu_bytes) { NULL, 0 }) == MIZU_ERR);
  assert(stage_rc == MIZU_ERR);
  assert(strstr(mizu_last_error_message(), "churn") != NULL);
  mizu_arrow_close(stage_arrow);

  mizu_channel_destroy(host);
  mizu_channel_destroy(peer);
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
  assert(ci.kind == MIZU_ARROW_DICT && ci.str_form == 1);
  mizu_arrow_col_info(a, 1, &ci);
  assert(ci.kind == MIZU_ARROW_TS);
  mizu_arrow_close(a);
}

/* The per-column writers: the body and dict slices match the MIZL frame
   write byte-for-byte on a non-verbatim column; on a verbatim-verdict
   column they emit the by-value form instead. */
static void test_col_writers(void) {
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
  mizu_arrow_warn warn;
  memset(&warn, 0, sizeof warn);

  mizu_arrow *a = NULL;
  assert(mizu_arrow_open(&a, &root, batches, 1, 0) == MIZU_OK);
  size_t total = mizu_arrow_size(a);
  uint8_t *frame = calloc(1, total);
  mizu_arrow_write(a, frame, &warn);

  /* the dict column: the 1-based codes and the factor dict */
  mizu_mizl_entry e;
  assert(mizu_mizl_elem(frame, total, 0, &e) == 0);
  size_t bn = mizu_arrow_col_body(a, 0, NULL, &warn);
  assert(bn == (size_t) e.data_size - (size_t) e.attrs_size);
  uint8_t *buf = calloc(1, bn + 64);
  assert(mizu_arrow_col_body(a, 0, buf, &warn) == bn);
  assert(memcmp(buf, frame + e.data_offset, bn) == 0);
  size_t dn = mizu_arrow_col_blob(a, 0, NULL);
  assert(dn == (size_t) e.attrs_size - mizu_ix_put_header(NULL));
  assert(mizu_arrow_col_blob(a, 0, buf) == dn);
  assert(memcmp(buf, frame + e.data_offset + bn +
                mizu_ix_put_header(NULL), dn) == 0);

  /* the timestamp column: the epoch-second doubles and the POSIXct dict */
  assert(mizu_mizl_elem(frame, total, 1, &e) == 0);
  bn = mizu_arrow_col_body(a, 1, NULL, &warn);
  assert(bn == (size_t) e.data_size - (size_t) e.attrs_size);
  assert(mizu_arrow_col_body(a, 1, buf, &warn) == bn);
  assert(memcmp(buf, frame + e.data_offset, bn) == 0);
  dn = mizu_arrow_col_blob(a, 1, NULL);
  assert(dn == (size_t) e.attrs_size - mizu_ix_put_header(NULL));
  assert(mizu_arrow_col_blob(a, 1, buf) == dn);
  assert(memcmp(buf, frame + e.data_offset + bn +
                mizu_ix_put_header(NULL), dn) == 0);

  free(buf);
  free(frame);
  mizu_arrow_close(a);

  /* the verbatim verdicts (peer_caps with TSN | U64V): the per-column
     writers still emit the by-value forms — second-seconds doubles, no
     tscale pair */
  ArrowSchema cn, cu, r2;
  ArrowSchema *kids2[2] = { &cn, &cu };
  mk_schema(&cn, "tsn:UTC", "t", NULL, 0, NULL, 0);
  mk_schema(&cu, "L", "u", NULL, 0, NULL, 0);
  mk_schema(&r2, "+s", NULL, kids2, 2, NULL, 0);
  const uint64_t uv[1] = { 42 };
  const void *ub[2] = { NULL, uv };
  ArrowArray an, au, b2;
  ArrowArray *bk2[2] = { &an, &au };
  mk_array(&an, 1, 0, 0, 2, tb, NULL, NULL);
  mk_array(&au, 1, 0, 0, 2, ub, NULL, NULL);
  mk_array(&b2, 1, 0, 0, 1, NULL, bk2, NULL);
  b2.n_children = 2;
  const ArrowArray *batches2[1] = { &b2 };
  assert(mizu_arrow_open(&a, &r2, batches2, 1,
                         MIZU_CAP_MIZL_TSN | MIZU_CAP_MIZL_U64V) == MIZU_OK);
  buf = calloc(1, 256);
  /* the ns column: 8 bytes of double seconds, the blob without tscale */
  assert(mizu_arrow_col_body(a, 0, buf, &warn) == 8);
  double sec;
  memcpy(&sec, buf, 8);
  assert(sec == 1000.0 * 1e-9);
  size_t zn = mizu_arrow_col_blob(a, 0, buf + 16);
  assert(zn != 0);
  assert(find_span(buf + 16, zn, "tscale", 6) == NULL);
  assert(find_span(buf + 16, zn, "UTC", 3) != NULL);
  /* the u64 column: 8 bytes of int64, no blob */
  assert(mizu_arrow_col_body(a, 1, buf, &warn) == 8);
  int64_t sv2;
  memcpy(&sv2, buf, 8);
  assert(sv2 == 42);
  assert(mizu_arrow_col_blob(a, 1, NULL) == 0);
  free(buf);
  mizu_arrow_close(a);

  /* a string column: the per-column writers return 0 — the binding's
     per-element 'I' emitter serves it */
  ArrowSchema cs, r3;
  ArrowSchema *kids3[1] = { &cs };
  mk_schema(&cs, "u", "s", NULL, 0, NULL, 0);
  mk_schema(&r3, "+s", NULL, kids3, 1, NULL, 0);
  const int32_t so[2] = { 0, 3 };
  const char sb[3] = { 'a', 'b', 'c' };
  const void *sv[3] = { NULL, so, sb };
  ArrowArray as, b3;
  ArrowArray *bk3[1] = { &as };
  mk_array(&as, 1, 0, 0, 3, sv, NULL, NULL);
  mk_array(&b3, 1, 0, 0, 1, NULL, bk3, NULL);
  b3.n_children = 1;
  const ArrowArray *batches3[1] = { &b3 };
  assert(mizu_arrow_open(&a, &r3, batches3, 1, 0) == MIZU_OK);
  assert(mizu_arrow_col_body(a, 0, NULL, &warn) == 0);
  assert(mizu_arrow_col_blob(a, 0, NULL) == 0);
  mizu_arrow_close(a);
}

// The inline small-table form (mizu_arrow_write_ix) -----------------------------

/* Frame A ("x" int32 {1, NA, 999}, "s" utf8 {"a", NA, "bc"}) as the
   complete 'I' stream — 148 bytes, the pin for byte identity with the
   bindings' historical inline emitters. */
static void expect_stream_a(uint8_t *e) {
  size_t o = 0;
  e[o] = 0x49; e[o + 1] = 0x01; o += 2;                 /* 'I' header */
  e[o] = MIZU_IX_TAG_ATTR; o += 1;
  e[o] = MIZU_IX_TAG_LIST; p64(e, o + 1, 2); o += 9;    /* list(2) */
  /* x: intv(3) {1, NA, 999} */
  e[o] = MIZU_IX_TAG_INTV; p64(e, o + 1, 3); o += 9;
  p32(e, o, 1); p32(e, o + 4, MIZU_NA_INT32); p32(e, o + 8, 999); o += 12;
  /* s: strv(3) {"a", NA, "bc"} (the null put as len -1) */
  e[o] = MIZU_IX_TAG_STRV; p64(e, o + 1, 3); o += 9;
  p32(e, o, 1); e[o + 4] = 'a'; o += 5;
  p32(e, o, -1); o += 4;
  p32(e, o, 2); raw(e, o + 4, "bc", 2); o += 6;
  /* the root dict {names, class, row.names} */
  e[o] = MIZU_IX_TAG_DICT; p64(e, o + 1, 3); o += 9;
  p32(e, o, 5); raw(e, o + 4, "names", 5); o += 9;
  e[o] = MIZU_IX_TAG_STRV; p64(e, o + 1, 2); o += 9;
  p32(e, o, 1); e[o + 4] = 'x'; o += 5;
  p32(e, o, 1); e[o + 4] = 's'; o += 5;
  p32(e, o, 5); raw(e, o + 4, "class", 5); o += 9;
  e[o] = MIZU_IX_TAG_STR; p32(e, o + 1, 10);
  raw(e, o + 5, "data.frame", 10); o += 15;
  p32(e, o, 9); raw(e, o + 4, "row.names", 9); o += 13;
  e[o] = MIZU_IX_TAG_INTV; p64(e, o + 1, 2); o += 9;
  p32(e, o, MIZU_NA_INT32); p32(e, o + 4, -3); o += 8;
  assert(o == 148);
}

static void test_write_ix(void) {
  /* frame A's arrays (test_frame_a) through the inline writer */
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

  mizu_arrow *a = NULL;
  assert(mizu_arrow_open(&a, &root, batches, 1, 0) == MIZU_OK);
  mizu_arrow_warn warn;
  memset(&warn, 0, sizeof warn);
  size_t n = mizu_arrow_write_ix(a, NULL, &warn);
  assert(n == 148);
  /* the size pass touches no counts */
  assert(warn.n_range == 0 && warn.n_intmin == 0 && warn.n_int64 == 0);
  uint8_t *buf = calloc(1, n);
  assert(buf != NULL);
  assert(mizu_arrow_write_ix(a, buf, &warn) == n);
  uint8_t *exp = calloc(1, n);
  assert(exp != NULL);
  expect_stream_a(exp);
  assert(memcmp(buf, exp, n) == 0);
  /* the stream parses end to end */
  blob_check(buf, n);
  /* the per-column writer: the slices match the frame stream (the root
     dict's 91 bytes follow them) */
  size_t off = 2 + 1 + 9;   /* header, attr, list begin */
  for (int64_t i = 0; i < 2; i++) {
    size_t cn = mizu_arrow_write_ix_col(a, i, NULL, &warn);
    assert(cn != 0);
    uint8_t *cbuf = calloc(1, cn);
    assert(cbuf != NULL);
    assert(mizu_arrow_write_ix_col(a, i, cbuf, &warn) == cn);
    assert(memcmp(cbuf, buf + off, cn) == 0);
    free(cbuf);
    off += cn;
  }
  assert(off == n - 91);
  assert(mizu_arrow_write_ix_col(a, 2, NULL, &warn) == 0);   /* in range */
  free(exp);
  free(buf);
  mizu_arrow_close(a);
}

static void test_write_ix_kinds(void) {
  /* one column per remaining kind: a dictionary, date32, timestamp[ms]
     with a zone, duration[s], int64, uint64 past INT64_MAX, string_view */
  ArrowSchema lv, cd, cdt, cts, ctd, ci, cu, cv, root;
  ArrowSchema *kids[7] = { &cd, &cdt, &cts, &ctd, &ci, &cu, &cv };
  mk_schema(&lv, "u", NULL, NULL, 0, NULL, 0);
  mk_schema(&cd, "c", "d", NULL, 0, &lv, 0);
  mk_schema(&cdt, "tdD", "dt", NULL, 0, NULL, 0);
  mk_schema(&cts, "tsm:UTC", "ts", NULL, 0, NULL, 0);
  mk_schema(&ctd, "tDs", "td", NULL, 0, NULL, 0);
  mk_schema(&ci, "l", "i64", NULL, 0, NULL, 0);
  mk_schema(&cu, "L", "u", NULL, 0, NULL, 0);
  mk_schema(&cv, "vu", "v", NULL, 0, NULL, 0);
  mk_schema(&root, "+s", NULL, kids, 7, NULL, 0);

  const char levbytes[2] = { 'a', 'b' };
  const int32_t levoffs[3] = { 0, 1, 2 };
  const int8_t codes[2] = { 0, 1 };
  const uint8_t dtbm = 0x01;   /* slot 1 null */
  const int32_t days[2] = { 19000, 0 };
  const int64_t tsv[2] = { 1000, 2000 };
  const int64_t tdv[2] = { 60, 120 };
  const int64_t i64v[2] = { 5, -7 };
  const uint64_t u64v[2] = { 18446744073709551615ULL, 3 };
  uint8_t views[32];
  memset(views, 0, sizeof views);
  int32_t l0 = 2, l1 = 20;
  memcpy(views, &l0, 4);
  views[4] = 'h'; views[5] = 'i';
  memcpy(views + 16, &l1, 4);
  char longs[20];
  memset(longs, 'z', sizeof longs);
  int32_t bi = 0, bo = 0;
  memcpy(views + 16 + 8, &bi, 4);
  memcpy(views + 16 + 12, &bo, 4);
  const void *db[3] = { NULL, levoffs, levbytes };
  const void *cb[2] = { NULL, codes };
  const void *dtb[2] = { &dtbm, days };
  const void *tsb[2] = { NULL, tsv };
  const void *tdb[2] = { NULL, tdv };
  const void *ib[2] = { NULL, i64v };
  const void *ub[2] = { NULL, u64v };
  const int64_t vsizes[1] = { (int64_t) sizeof longs };
  const void *vb[4] = { NULL, views, longs, vsizes };
  ArrowArray dict, ad, adt, ats, atd, ai, au, av, batch;
  ArrowArray *bk[7] = { &ad, &adt, &ats, &atd, &ai, &au, &av };
  mk_array(&dict, 2, 0, 0, 3, db, NULL, NULL);
  mk_array(&ad, 2, 0, 0, 2, cb, NULL, &dict);
  mk_array(&adt, 2, 1, 0, 2, dtb, NULL, NULL);
  mk_array(&ats, 2, 0, 0, 2, tsb, NULL, NULL);
  mk_array(&atd, 2, 0, 0, 2, tdb, NULL, NULL);
  mk_array(&ai, 2, 0, 0, 2, ib, NULL, NULL);
  mk_array(&au, 2, 0, 0, 2, ub, NULL, NULL);
  mk_array(&av, 2, 0, 0, 4, vb, NULL, NULL);
  mk_array(&batch, 2, 0, 0, 1, NULL, bk, NULL);
  batch.n_children = 7;
  const ArrowArray *batches[1] = { &batch };

  mizu_arrow *a = NULL;
  assert(mizu_arrow_open(&a, &root, batches, 1, 0) == MIZU_OK);
  mizu_arrow_warn warn;
  memset(&warn, 0, sizeof warn);
  size_t n = mizu_arrow_write_ix(a, NULL, &warn);
  uint8_t *buf = calloc(1, n);
  assert(buf != NULL);
  assert(mizu_arrow_write_ix(a, buf, &warn) == n);
  assert(warn.n_range == 1 && warn.n_intmin == 0 && warn.n_int64 == 0);
  blob_check(buf, n);

  /* walk the column slices */
  size_t off = 2 + 1 + 9;
  size_t csz[7];
  for (int64_t i = 0; i < 7; i++) {
    csz[i] = mizu_arrow_write_ix_col(a, i, NULL, &warn);
    assert(csz[i] != 0);
    off += csz[i];
  }
  assert(off < n);
  /* d: attr + intv {1, 2} + the bare factor dict (no stream header) */
  const uint8_t *s = buf + 2 + 1 + 9;
  assert(s[0] == MIZU_IX_TAG_ATTR && s[1] == MIZU_IX_TAG_INTV);
  int32_t code[2];
  memcpy(code, s + 10, 8);
  assert(code[0] == 1 && code[1] == 2);
  assert(s[18] == MIZU_IX_TAG_DICT);
  assert(find_span(s, csz[0], "levels", 6) != NULL);
  assert(find_span(s, csz[0], "factor", 6) != NULL);
  /* dt: attr + realv {19000.0, the R NA payload} + the Date dict */
  s += csz[0];
  assert(s[0] == MIZU_IX_TAG_ATTR && s[1] == MIZU_IX_TAG_REALV);
  double dv[2];
  memcpy(dv, s + 10, 16);
  uint64_t dbits;
  memcpy(&dbits, &dv[1], 8);
  assert(dv[0] == 19000.0 && dbits == 0x7FF00000000007A2ULL);
  assert(find_span(s, csz[1], "Date", 4) != NULL);
  /* ts: realv epoch seconds; the dict names the zone, never tscale */
  s += csz[1];
  assert(s[0] == MIZU_IX_TAG_ATTR && s[1] == MIZU_IX_TAG_REALV);
  memcpy(dv, s + 10, 16);
  assert(dv[0] == 1.0 && dv[1] == 2.0);
  assert(find_span(s, csz[2], "POSIXct", 7) != NULL);
  assert(find_span(s, csz[2], "UTC", 3) != NULL);
  assert(find_span(s, csz[2], "tscale", 6) == NULL);
  /* td: realv seconds; the difftime dict with units */
  s += csz[2];
  assert(s[0] == MIZU_IX_TAG_ATTR && s[1] == MIZU_IX_TAG_REALV);
  memcpy(dv, s + 10, 16);
  assert(dv[0] == 60.0 && dv[1] == 120.0);
  assert(find_span(s, csz[3], "difftime", 8) != NULL);
  assert(find_span(s, csz[3], "secs", 4) != NULL);
  /* i64: the i64v tag, the lanes verbatim */
  s += csz[3];
  assert(s[0] == MIZU_IX_TAG_I64V);
  int64_t lanes[2];
  memcpy(lanes, s + 9, 16);
  assert(lanes[0] == 5 && lanes[1] == -7);
  /* u: the integer64 conversion, past-INT64_MAX -> the NA sentinel */
  s += csz[4];
  assert(s[0] == MIZU_IX_TAG_I64V);
  memcpy(lanes, s + 9, 16);
  assert(lanes[0] == MIZU_NA_INT64 && lanes[1] == 3);
  /* v: strv, the inline short string and the buffered long one */
  s += csz[5];
  assert(s[0] == MIZU_IX_TAG_STRV);
  int32_t sl0, sl1;
  memcpy(&sl0, s + 9, 4);
  memcpy(&sl1, s + 9 + 4 + 2, 4);
  assert(sl0 == 2 && memcmp(s + 13, "hi", 2) == 0);
  assert(sl1 == 20 && memcmp(s + 19, longs, 20) == 0);
  /* the root dict names all seven */
  assert(find_span(buf + off, n - off, "i64", 3) != NULL);
  free(buf);
  mizu_arrow_close(a);

  /* the one-column frame: the names value takes the scalar form */
  ArrowSchema cs2, r2;
  ArrowSchema *kids2[1] = { &cs2 };
  mk_schema(&cs2, "u", "s", NULL, 0, NULL, 0);
  mk_schema(&r2, "+s", NULL, kids2, 1, NULL, 0);
  const int32_t so[2] = { 0, 3 };
  const char sb[3] = { 'a', 'b', 'c' };
  const void *sv[3] = { NULL, so, sb };
  ArrowArray as, b2;
  ArrowArray *bk2[1] = { &as };
  mk_array(&as, 1, 0, 0, 3, sv, NULL, NULL);
  mk_array(&b2, 1, 0, 0, 1, NULL, bk2, NULL);
  b2.n_children = 1;
  const ArrowArray *batches2[1] = { &b2 };
  assert(mizu_arrow_open(&a, &r2, batches2, 1, 0) == MIZU_OK);
  n = mizu_arrow_write_ix(a, NULL, &warn);
  buf = calloc(1, n);
  assert(buf != NULL);
  assert(mizu_arrow_write_ix(a, buf, &warn) == n);
  blob_check(buf, n);
  /* the scalar str tag ahead of the one name's length */
  const uint8_t *nm = (const uint8_t *) find_span(buf, n, "\x04\x01\x00\x00\x00" "s", 6);
  assert(nm != NULL);
  free(buf);
  mizu_arrow_close(a);

  /* the zero-row frame: a parseable stream, row.names c(NA, 0) */
  ArrowSchema ci0, r0;
  ArrowSchema *kids0[1] = { &ci0 };
  mk_schema(&ci0, "i", "x", NULL, 0, NULL, 0);
  mk_schema(&r0, "+s", NULL, kids0, 1, NULL, 0);
  assert(mizu_arrow_open(&a, &r0, NULL, 0, 0) == MIZU_OK);
  n = mizu_arrow_write_ix(a, NULL, &warn);
  buf = calloc(1, n);
  assert(buf != NULL);
  assert(mizu_arrow_write_ix(a, buf, &warn) == n);
  blob_check(buf, n);
  free(buf);
  mizu_arrow_close(a);
}

int main(void) {
  test_frame_a();
  test_cvt_types();
  test_intmin();
  test_intmin_unmasked();
  test_masked_unaligned();
  test_unaligned_buffers();
  test_zero_rows();
  test_chunked_and_offsets();
  test_null_span_collapse();
  test_null_count_forms();
  test_nan_vs_na();
  test_string_forms();
  test_strview_buffer_bound();
  test_str_offsets_malformed();
  test_dicts();
  test_dict_declines();
  test_dict_batch_shape();
  test_temporal();
  test_temporal_ns();
  test_uint64_verbatim();
  test_declines();
  test_short_formats();
  test_struct_batch_shape();
  test_cvt_kernels();
  test_open_declines2();
  test_rows_bound();
  test_dict_widths();
  test_dict_edge_shapes();
  test_dict_strview_inline();
  test_remote_decline();
  test_remote();
  test_root_blob_override();
  test_tz_override();
  test_override_after_size();
  test_stage();
  test_col_info();
  test_col_writers();
  test_write_ix();
  test_write_ix_kinds();
  printf("test_arrow: OK\n");
  return 0;
}
