/* Unit tier: the mizu_mizh_ / mizu_mizs_ / mizu_mizl_ layout checks
   (accept written regions, reject truncated or corrupt ones), the
   validity-section setter, the string-block geometry, and the NA
   build/apply primitives. Hand-stamped byte layouts stand in for the
   bindings' writers. Assert-based; run via `make test`. */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "mizu_ext.h"

static void put32(unsigned char *base, size_t off, uint32_t v) {
  memcpy(base + off, &v, 4);
}

static void put64(unsigned char *base, size_t off, int64_t v) {
  memcpy(base + off, &v, 8);
}

static void flags_word(unsigned char *base, uint32_t flags) {
  put32(base, MIZU_HDR_FLAGS_OFF, flags);
}

/* A two-entry MIZL region: REAL(3) leaf at 128, STR(2) block at 192, an
   optional validity table at 448 and bitmap at 512. Region is 576
   bytes. */
static void write_mizl(unsigned char *base) {
  memset(base, 0, 576);
  put32(base, 0, MIZU_MAGIC_LIST);
  put32(base, 4, 2);
  put64(base, 8, 384);                     /* attrs_off */
  put64(base, 16, 0);                      /* attrs_size */
  mizu_mizl_entry e0 = { 128, 24, MIZU_TYPE_REAL, 0, 3, { 0, 0 } };
  memcpy(base + 64, &e0.data_offset, 8);
  memcpy(base + 64 + 8, &e0.data_size, 8);
  memcpy(base + 64 + 16, &e0.sexptype, 4);
  memcpy(base + 64 + 20, &e0.attrs_size, 4);
  memcpy(base + 64 + 24, &e0.length, 8);
  const int64_t str_size = mizu_mizs_geometry(2).data + 5;
  mizu_mizl_entry e1 = { 192, str_size, MIZU_TYPE_STR, 0, 2, { 0, 0 } };
  memcpy(base + 96, &e1.data_offset, 8);
  memcpy(base + 96 + 8, &e1.data_size, 8);
  memcpy(base + 96 + 16, &e1.sexptype, 4);
  memcpy(base + 96 + 20, &e1.attrs_size, 4);
  memcpy(base + 96 + 24, &e1.length, 8);
}

static void mizh_tests(void) {
  unsigned char r[192];
  memset(r, 0xAA, sizeof r);
  mizu_mizh_write(r, MIZU_TYPE_REAL, 3);
  int type = 0;
  int64_t n = 0, valid[2] = { 1, 1 };
  assert(mizu_mizh_check(r, sizeof r, &type, &n, valid) == 0);
  assert(type == MIZU_TYPE_REAL && n == 3);
  assert(valid[0] == 0 && valid[1] == 0);

  /* truncated */
  assert(mizu_mizh_check(r, MIZU_HEADER_SIZE - 1, &type, &n, valid) != 0);
  /* an unknown set bit in the flags word rejects; S4 (bit 0) passes */
  flags_word(r, 2u);
  assert(mizu_mizh_check(r, sizeof r, &type, &n, valid) != 0);
  flags_word(r, MIZU_HDR_FLAG_S4);
  assert(mizu_mizh_check(r, sizeof r, &type, &n, valid) == 0);
  flags_word(r, 0u);

  /* the validity pair's three states */
  mizu_mizh_validity_set(r, 0, -1);
  assert(mizu_mizh_check(r, sizeof r, &type, &n, valid) == 0 &&
         valid[0] == 0 && valid[1] == -1);
  mizu_mizh_validity_set(r, 0, 1);              /* {0, count} is neither */
  assert(mizu_mizh_check(r, sizeof r, &type, &n, valid) != 0);
  mizu_mizh_validity_set(r, 8, 0);              /* misaligned offset */
  assert(mizu_mizh_check(r, sizeof r, &type, &n, valid) != 0);
  mizu_mizh_validity_set(r, 4096, 0);           /* out of region */
  assert(mizu_mizh_check(r, sizeof r, &type, &n, valid) != 0);
  mizu_mizh_validity_set(r, 192, 0);            /* 1 bitmap byte needed */
  assert(mizu_mizh_check(r, sizeof r, &type, &n, valid) != 0);
  mizu_mizh_validity_set(r, 128, 4);            /* count past n */
  assert(mizu_mizh_check(r, sizeof r, &type, &n, valid) != 0);
  mizu_mizh_validity_set(r, 128, 1);            /* fits: 64 + 24 + 64 */
  assert(mizu_mizh_check(r, sizeof r, &type, &n, valid) == 0 &&
         valid[0] == 128 && valid[1] == 1);
}

static void mizs_tests(void) {
  /* geometry: section offsets for n strings */
  mizu_mizs_geom g0 = mizu_mizs_geometry(0);
  assert(g0.validity == 0 && g0.offsets == 0 && g0.encoding == 64 &&
         g0.data == 64);
  mizu_mizs_geom g1 = mizu_mizs_geometry(1);
  assert(g1.offsets == 64 && g1.encoding == 128 && g1.data == 192);
  mizu_mizs_geom g2 = mizu_mizs_geometry(2);
  assert(g2.offsets == 64 && g2.encoding == 128 && g2.data == 192);

  unsigned char r[320];
  memset(r, 0, sizeof r);
  const int64_t str_size = g2.data + 5;         /* "hello" packed */
  put32(r, 0, MIZU_MAGIC_STR);
  put32(r, 4, 7);                               /* attrs_size */
  put64(r, 8, 2);                               /* n */
  put64(r, 16, str_size);
  int64_t n = 0, block = 0, attrs = 0;
  assert(mizu_mizs_check(r, 64 + str_size + 7, &n, &block, &attrs) == 0);
  assert(n == 2 && block == str_size && attrs == 7);

  assert(mizu_mizs_check(r, 63, &n, &block, &attrs) != 0);
  flags_word(r, 4u);                            /* an unknown bit */
  assert(mizu_mizs_check(r, 64 + str_size + 7, &n, &block, &attrs) != 0);
  flags_word(r, 0u);
  /* the block end and the attrs end against the region */
  assert(mizu_mizs_check(r, 64 + str_size + 6, &n, &block, &attrs) != 0);
  put64(r, 16, g2.data - 1);                    /* fixed sections short */
  assert(mizu_mizs_check(r, sizeof r, &n, &block, &attrs) != 0);
  put64(r, 16, 64 + str_size + 7);              /* block past the region */
  assert(mizu_mizs_check(r, 64 + str_size + 7, &n, &block, &attrs) != 0);
}

static void mizl_tests(void) {
  unsigned char r[576];
  write_mizl(r);
  int64_t n = 0, aoff = 0, asz = 0, valid[2] = { 1, 1 };
  assert(mizu_mizl_check(r, sizeof r, &n, &aoff, &asz, valid) == 0);
  assert(n == 2 && aoff == 384 && asz == 0);
  assert(valid[0] == 0 && valid[1] == 0);

  mizu_mizl_entry e;
  assert(mizu_mizl_elem(r, sizeof r, 0, &e) == 0);
  assert(e.data_offset == 128 && e.data_size == 24 &&
         e.sexptype == MIZU_TYPE_REAL && e.length == 3 &&
         e.valid[0] == 0 && e.valid[1] == 0);
  assert(mizu_mizl_elem(r, sizeof r, 1, &e) == 0);
  assert(e.sexptype == MIZU_TYPE_STR && e.length == 2);
  assert(mizu_mizl_elem(r, sizeof r, 2, &e) != 0);   /* past n */

  /* a misaligned directory data_offset rejects, in both calls */
  put64(r, 64, 136);
  assert(mizu_mizl_check(r, sizeof r, &n, &aoff, &asz, valid) != 0);
  assert(mizu_mizl_elem(r, sizeof r, 0, &e) != 0);
  put64(r, 64, 128);

  /* an unknown flags-word bit rejects */
  flags_word(r, 0x80000000u);
  assert(mizu_mizl_check(r, sizeof r, &n, &aoff, &asz, valid) != 0);
  flags_word(r, 0u);

  /* an unlisted sexptype rejects; the reserved remote leaf (33) with it;
     the S4 bit rides the tag */
  put32(r, 64 + 16, 33);
  assert(mizu_mizl_check(r, sizeof r, &n, &aoff, &asz, valid) != 0);
  put32(r, 64 + 16, 7);
  assert(mizu_mizl_check(r, sizeof r, &n, &aoff, &asz, valid) != 0);
  put32(r, 64 + 16, (uint32_t) MIZU_TYPE_REAL | MIZU_MIZL_S4);
  assert(mizu_mizl_check(r, sizeof r, &n, &aoff, &asz, valid) == 0);
  assert(mizu_mizl_elem(r, sizeof r, 0, &e) == 0 &&
         (e.sexptype & MIZU_MIZL_S4) != 0);
  put32(r, 64 + 16, MIZU_TYPE_REAL);

  /* the header validity table: {0, -1} covers every leaf */
  mizu_mizh_validity_set(r, 0, -1);
  assert(mizu_mizl_check(r, sizeof r, &n, &aoff, &asz, valid) == 0 &&
         valid[0] == 0 && valid[1] == -1);
  assert(mizu_mizl_elem(r, sizeof r, 1, &e) == 0 && e.valid[1] == -1);

  /* a present table at 448: REAL leaf bitmap at 512, STR leaf {0, 0} */
  mizu_mizh_validity_set(r, 448, 1);
  put64(r, 448, 512);
  put64(r, 456, 1);
  put64(r, 464, 0);
  put64(r, 472, 0);
  assert(mizu_mizl_check(r, sizeof r, &n, &aoff, &asz, valid) == 0 &&
         valid[0] == 448 && valid[1] == 1);
  assert(mizu_mizl_elem(r, sizeof r, 0, &e) == 0 &&
         e.valid[0] == 512 && e.valid[1] == 1);
  assert(mizu_mizl_elem(r, sizeof r, 1, &e) == 0 &&
         e.valid[0] == 0 && e.valid[1] == 0);

  /* a header total above the directory entry count is accepted... */
  mizu_mizh_validity_set(r, 448, 3);            /* 3 > n = 2, <= sum 3 */
  assert(mizu_mizl_check(r, sizeof r, &n, &aoff, &asz, valid) == 0);
  /* ...but not past the leaf-length sum */
  mizu_mizh_validity_set(r, 448, 4);
  assert(mizu_mizl_check(r, sizeof r, &n, &aoff, &asz, valid) != 0);
  mizu_mizh_validity_set(r, 447, 1);            /* misaligned table */
  assert(mizu_mizl_check(r, sizeof r, &n, &aoff, &asz, valid) != 0);
  mizu_mizh_validity_set(r, 448, 1);

  /* a leaf entry: bitmap out of region, misaligned, count past length */
  put64(r, 448, 65536);
  assert(mizu_mizl_elem(r, sizeof r, 0, &e) != 0);
  put64(r, 448, 576);                           /* aligned but no room */
  assert(mizu_mizl_elem(r, sizeof r, 0, &e) != 0);
  put64(r, 448, 513);                           /* misaligned */
  assert(mizu_mizl_elem(r, sizeof r, 0, &e) != 0);
  put64(r, 448, 512);
  put64(r, 456, 4);                             /* count past length 3 */
  assert(mizu_mizl_elem(r, sizeof r, 0, &e) != 0);
}

static void na_tests(void) {
  uint8_t bm[8];

  /* INT / LGL share the INT32_MIN sentinel; build writes set and clear */
  int32_t vi[4] = { 1, INT32_MIN, -7, INT32_MIN };
  memset(bm, 0xFF, sizeof bm);
  assert(mizu_na_build(MIZU_TYPE_INT, bm, vi, 4, 0) == 2);
  assert(bm[0] == 0xF5);                        /* bits 1, 3 cleared */
  memset(bm, 0, sizeof bm);
  assert(mizu_na_build(MIZU_TYPE_LGL, bm, vi, 4, 0) == 2);
  assert(bm[0] == 0x05);                        /* bits 0, 2 set */
  int32_t di[4] = { 0, 0, 0, 0 };
  assert(mizu_na_apply(MIZU_TYPE_INT, di, vi, bm, 4) == 2);
  assert(memcmp(di, vi, sizeof di) == 0);

  /* REAL: the NA_real_ payload is discriminated from other NaNs */
  uint64_t vr[4] = { 0x3FF0000000000000ULL,     /* 1.0 */
                     MIZU_NA_REAL_BITS,
                     0x7FF8000000000001ULL,     /* another NaN payload */
                     MIZU_NA_REAL_BITS };
  memset(bm, 0, sizeof bm);
  assert(mizu_na_build(MIZU_TYPE_REAL, bm, vr, 4, 0) == 2);
  assert(bm[0] == 0x05);
  double dr[4];
  assert(mizu_na_apply(MIZU_TYPE_REAL, dr, vr, bm, 4) == 2);
  assert(memcmp(dr, vr, sizeof dr) == 0);

  /* CPLX: either part carrying the payload */
  uint64_t vc[4] = { MIZU_NA_REAL_BITS, 0,      /* re NA */
                     0x7FF8000000000001ULL, 0x3FF0000000000000ULL };
  memset(bm, 0, sizeof bm);
  assert(mizu_na_build(MIZU_TYPE_CPLX, bm, vc, 2, 0) == 1);
  assert(bm[0] == 0x02);                        /* element 0 null, 1 ok */
  uint64_t dc[4] = { 0, 0, 0, 0 };
  assert(mizu_na_apply(MIZU_TYPE_CPLX, dc, vc, bm, 2) == 1);
  /* the canonical sentinel writes both parts; other elements verbatim */
  assert(dc[0] == MIZU_NA_REAL_BITS && dc[1] == MIZU_NA_REAL_BITS &&
         dc[2] == vc[2] && dc[3] == vc[3]);

  /* INT64_MIN, and a bit_off build */
  int64_t vx[3] = { 5, INT64_MIN, -9 };
  memset(bm, 0, sizeof bm);
  assert(mizu_na_build(MIZU_TYPE_INT64, bm, vx, 3, 3) == 1);
  assert(bm[0] == 0x28);                        /* bits 3 and 5 set */
  int64_t dx[3] = { 0, 0, 0 };
  uint8_t one[1] = { 0x06 };                    /* element 0 null, 1, 2 ok */
  assert(mizu_na_apply(MIZU_TYPE_INT64, dx, vx, one, 3) == 1);
  assert(dx[0] == INT64_MIN && dx[1] == INT64_MIN && dx[2] == -9);

  /* RAW has no missing sentinel: all-present, never a null */
  uint8_t vraw[3] = { 0, 1, 255 };
  memset(bm, 0, sizeof bm);
  assert(mizu_na_build(MIZU_TYPE_RAW, bm, vraw, 3, 0) == 0);
  assert(bm[0] == 0x07);

  /* build/apply round trip reproduces the sentinel positions */
  int32_t src[5] = { INT32_MIN, 2, INT32_MIN, 4, 5 };
  int32_t dst[5] = { 0, 0, 0, 0, 0 };
  memset(bm, 0, sizeof bm);
  assert(mizu_na_build(MIZU_TYPE_INT, bm, src, 5, 0) == 2);
  assert(mizu_na_apply(MIZU_TYPE_INT, dst, src, bm, 5) == 2);
  assert(memcmp(dst, src, sizeof dst) == 0);
}

int main(void) {
  mizh_tests();
  mizs_tests();
  mizl_tests();
  na_tests();
  printf("test_layout: OK\n");
  return 0;
}
