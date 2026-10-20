/* Unit tier: the dual-form wire helpers (mizu_ext.h) — timeout conversion
   edges, the NA stores, the aux pack identities, the MIZH header
   write/check round trip, and the MIZS/MIZL/Arrow readers' rejection
   cases. Assert-based; run via `make test`. */

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mizu.h"
#include "mizu_ext.h"

int main(void) {
  /* mizu_timeout_ms: non-finite waits indefinitely, <= 0 polls. */
  assert(mizu_timeout_ms(NAN) == -1);
  assert(mizu_timeout_ms(INFINITY) == -1);
  assert(mizu_timeout_ms(-INFINITY) == -1);
  assert(mizu_timeout_ms(0) == 0);
  assert(mizu_timeout_ms(-2.5) == 0);
  assert(mizu_timeout_ms(1) == 1000);
  assert(mizu_timeout_ms(0.001) == 1);
  assert(mizu_timeout_ms(1e-6) == 0.001);   /* sub-ms stays fractional */
  /* a finite seconds whose ms product overflows reports indefinite (the
     wait is unbounded either way) — no Inf escapes into the verbs */
  assert(mizu_timeout_ms(1e306) == -1);
  assert(mizu_timeout_ms(1e305) == 1e308);

  /* The NA stores and constants. */
  assert(MIZU_NA_INT32 == INT32_MIN);
  uint64_t na64 = MIZU_NA_INT64;
  assert(na64 == (uint64_t) INT64_MIN);
  unsigned char buf[8];
  mizu_store_na_real(buf);
  uint64_t bits;
  memcpy(&bits, buf, 8);
  assert(bits == MIZU_NA_REAL_BITS);
  double na_real;
  memcpy(&na_real, buf, 8);
  assert(isnan(na_real));

  /* aux pack/unpack identities (mizu.h's kind table). */
  assert(mizu_aux_type(mizu_aux_rawspill_pool(MIZU_TYPE_INT64, 27)) == 32);
  assert(mizu_aux_hi(mizu_aux_rawspill_pool(MIZU_TYPE_INT64, 27)) == 27);
  assert(mizu_aux_rawspill_pool(MIZU_TYPE_REAL, 255) ==
         ((uint64_t) MIZU_TYPE_REAL | ((uint64_t) 255 << 8)));
  assert(mizu_aux_type(mizu_aux_shm_vec(MIZU_TYPE_STR, 65536 + 64)) == 16);
  assert(mizu_aux_hi(mizu_aux_shm_vec(MIZU_TYPE_STR, 65536 + 64)) == 65600);

  /* MIZH round trip. */
  unsigned char mizh[256];
  memset(mizh, 0xAA, sizeof mizh);
  mizu_mizh_write(mizh, MIZU_TYPE_INT, 42);
  int type = 0;
  int64_t n = 0, valid[2];
  assert(mizu_mizh_check(mizh, sizeof mizh, &type, &n, valid) == 0);
  assert(type == MIZU_TYPE_INT && n == 42);
  for (size_t i = 24; i < MIZU_HEADER_SIZE; i++)
    assert(mizh[i] == 0);   /* the reserved band, zc words included */

  /* Rejections: size, magic, type, extent. */
  assert(mizu_mizh_check(mizh, MIZU_HEADER_SIZE - 1, &type, &n, valid) ==
         -1);
  unsigned char bad[256];
  memcpy(bad, mizh, sizeof bad);
  bad[0] ^= 0xFF;   /* magic */
  assert(mizu_mizh_check(bad, sizeof bad, &type, &n, valid) == -1);
  memcpy(bad, mizh, sizeof bad);
  bad[4] = 7;       /* no such atomic wire type */
  assert(mizu_mizh_check(bad, sizeof bad, &type, &n, valid) == -1);
  memcpy(bad, mizh, sizeof bad);
  mizu_mizh_write(bad, MIZU_TYPE_REAL, 1 << 20);   /* 8 MB into 128 B */
  assert(mizu_mizh_check(bad, sizeof bad, &type, &n, valid) == -1);
  memcpy(bad, mizh, sizeof bad);
  bad[16] = 0xFF;   /* attrs_size past the region */
  assert(mizu_mizh_check(bad, sizeof bad, &type, &n, valid) == -1);
  /* a negative element count rejects */
  memcpy(bad, mizh, sizeof bad);
  int64_t neg = -1;
  memcpy(bad + 8, &neg, 8);
  assert(mizu_mizh_check(bad, sizeof bad, &type, &n, valid) == -1);

  /* mizu_ext_mizl_tag_ok: a serialized leaf (0) is a listed tag. */
  assert(mizu_ext_mizl_tag_ok(0) == 1);

  /* mizu_ext_na_store: the int32 arm; a non-NA type's no-op. */
  unsigned char nb[16];
  mizu_ext_na_store(MIZU_TYPE_LGL, nb, 1);
  int32_t nv;
  memcpy(&nv, nb + 4, 4);
  assert(nv == MIZU_NA_INT32);
  memset(nb, 0x5A, sizeof nb);
  mizu_ext_na_store(MIZU_TYPE_STR, nb, 0);
  assert(nb[0] == 0x5A);   /* untouched */

  /* MIZS: a negative string count rejects. */
  unsigned char mizs[128];
  memset(mizs, 0, sizeof mizs);
  uint32_t magic = MIZU_MAGIC_STR;
  memcpy(mizs, &magic, 4);
  int64_t mcount = -1;
  memcpy(mizs + 8, &mcount, 8);
  int64_t sn = 0, sblock = 0, sattrs = 0;
  assert(mizu_mizs_check(mizs, sizeof mizs, &sn, &sblock, &sattrs) == -1);

  /* MIZL headers: a negative entry count; a negative attrs offset. */
  unsigned char mizl[192];
  memset(mizl, 0, sizeof mizl);
  magic = MIZU_MAGIC_LIST;
  memcpy(mizl, &magic, 4);
  int64_t ln = 0, laoff = 0, lasz = 0, lvalid[2];
  int32_t n32 = -1;
  memcpy(mizl + 4, &n32, 4);
  assert(mizu_mizl_check(mizl, sizeof mizl, &ln, &laoff, &lasz, lvalid) ==
         -1);
  n32 = 0;
  memcpy(mizl + 4, &n32, 4);
  int64_t aoff = -1;
  memcpy(mizl + 8, &aoff, 8);
  assert(mizu_mizl_check(mizl, sizeof mizl, &ln, &laoff, &lasz, lvalid) ==
         -1);

  /* MIZL directory entries: a string leaf whose block is short of the
     fixed sections rejects; a list leaf's negative length rejects. */
  memset(mizl, 0, sizeof mizl);
  memcpy(mizl, &magic, 4);
  n32 = 1;
  memcpy(mizl + 4, &n32, 4);
  int64_t eoff = 64, esz = 8, elen = 1;
  int32_t etag = MIZU_TYPE_STR, eattrs = 0;
  unsigned char *dir = mizl + MIZU_HEADER_SIZE;
  memcpy(dir, &eoff, 8);
  memcpy(dir + 8, &esz, 8);
  memcpy(dir + 16, &etag, 4);
  memcpy(dir + 20, &eattrs, 4);
  memcpy(dir + 24, &elen, 8);
  assert(mizu_mizl_check(mizl, sizeof mizl, &ln, &laoff, &lasz, lvalid) ==
         -1);
  etag = MIZU_TYPE_VEC;
  elen = -1;
  memcpy(dir + 16, &etag, 4);
  memcpy(dir + 24, &elen, 8);
  assert(mizu_mizl_check(mizl, sizeof mizl, &ln, &laoff, &lasz, lvalid) ==
         -1);

  /* mizu_mizl_elem: a validity-table offset past the region rejects
     (the leaf itself is valid). */
  etag = MIZU_TYPE_REAL;
  elen = 1;
  memcpy(dir + 16, &etag, 4);
  memcpy(dir + 24, &elen, 8);
  int64_t voff = 192;   /* 64-aligned, past the 128-byte extent given */
  memcpy(mizl + MIZU_HDR_VALID_OFF, &voff, 8);
  mizu_mizl_entry ent;
  assert(mizu_mizl_elem(mizl, 128, 0, &ent) == -1);

  /* STR / VEC / serialized leaves carry no validity-table bitmap (the
     string block, the nested header and the blob speak for themselves;
     the writers stamp those rows {0,0}): a bitmap row on one rejects,
     while the same row on an atomic leaf passes. */
  unsigned char ml2[512];
  memset(ml2, 0, sizeof ml2);
  memcpy(ml2, &magic, 4);               /* MIZU_MAGIC_LIST, 1 entry */
  memcpy(ml2 + 4, &n32, 4);
  int64_t tab = 256, tbmp = 64, tcnt = 1;
  memcpy(ml2 + MIZU_HDR_VALID_OFF, &tab, 8);
  memcpy(ml2 + 256, &tbmp, 8);
  memcpy(ml2 + 264, &tcnt, 8);
  unsigned char *d2 = ml2 + MIZU_HEADER_SIZE;
  int64_t eoff2 = 64, elen2 = 1;
  int64_t esz2 = mizu_mizs_geometry(1).data;
  int32_t etag2 = MIZU_TYPE_STR, eattrs2 = 0;
  memcpy(d2, &eoff2, 8);
  memcpy(d2 + 8, &esz2, 8);
  memcpy(d2 + 16, &etag2, 4);
  memcpy(d2 + 20, &eattrs2, 4);
  memcpy(d2 + 24, &elen2, 8);
  assert(mizu_mizl_elem(ml2, sizeof ml2, 0, &ent) == -1);   /* STR */
  etag2 = MIZU_TYPE_VEC;
  esz2 = 64;
  memcpy(d2 + 8, &esz2, 8);
  memcpy(d2 + 16, &etag2, 4);
  assert(mizu_mizl_elem(ml2, sizeof ml2, 0, &ent) == -1);   /* VEC */
  etag2 = 0;
  memcpy(d2 + 16, &etag2, 4);
  assert(mizu_mizl_elem(ml2, sizeof ml2, 0, &ent) == -1);   /* serialized */
  etag2 = MIZU_TYPE_REAL;
  esz2 = 8;
  memcpy(d2 + 8, &esz2, 8);
  memcpy(d2 + 16, &etag2, 4);
  assert(mizu_mizl_elem(ml2, sizeof ml2, 0, &ent) == 0);    /* atomic passes */

  /* mizu_mizl_check: the leaf-length sum bounding the header validity
     count saturates instead of wrapping — a crafted region whose
     overlapping leaves sum past 2^64 must not over-reject a count the
     true sum admits. */
  unsigned char big[4096];
  memset(big, 0, sizeof big);
  memcpy(big, &magic, 4);
  int32_t bn = 100;
  memcpy(big + 4, &bn, 4);
  int64_t boff = 0, blen = (int64_t) 1 << 59;
  int64_t bsz = (int64_t) 1 << 62;
  int32_t btag = MIZU_TYPE_REAL, battrs = 0;
  for (int i = 0; i < bn; i++) {
    unsigned char *d = big + MIZU_HEADER_SIZE + 32 * i;
    memcpy(d, &boff, 8);
    memcpy(d + 8, &bsz, 8);
    memcpy(d + 16, &btag, 4);
    memcpy(d + 20, &battrs, 4);
    memcpy(d + 24, &blen, 8);
  }
  int64_t bvoff = 4096, bvc = INT64_MAX;
  memcpy(big + MIZU_HDR_VALID_OFF, &bvoff, 8);
  memcpy(big + MIZU_HDR_VALID_COUNT, &bvc, 8);
  {
    int64_t rn = 0, raoff = 0, rasz = 0, rv[2];
    assert(mizu_mizl_check(big, (size_t) ((uint64_t) 1 << 63), &rn, &raoff,
                           &rasz, rv) == 0);
  }

  /* mizu_arrow_str_at form 2 (large_utf8): 64-bit offsets, with the
     negative-offset rejection. */
  int64_t aoffs[2] = { 0, 3 };
  const void *abufs[3] = { NULL, aoffs, "abc" };
  ArrowArray aa;
  memset(&aa, 0, sizeof aa);
  aa.length = 1;
  aa.n_buffers = 3;
  aa.buffers = abufs;
  const uint8_t *sp = NULL;
  int32_t sl = 0;
  assert(mizu_arrow_str_at(&aa, 0, 2, &sp, &sl) == 1);
  assert(sl == 3 && memcmp(sp, "abc", 3) == 0);
  aoffs[0] = -1;
  assert(mizu_arrow_str_at(&aa, 0, 2, &sp, &sl) == -1);

  /* mizu_arrow_str_at form 3 (string_view): the spec layout's trailing
     sizes array bounds a span; a view naming the trailing slot proves a
     no-sizes export (nanoarrow's nested or standalone form) and crosses
     unbounded. */
  const char vdata[] = "alpha alpha alpha alphabeta beta beta beta beta";
  unsigned char vsizes[32];
  memset(vsizes, 0, sizeof vsizes);
  int64_t s0 = 47;
  memcpy(vsizes, &s0, 8);
  uint8_t views[16];
  memset(views, 0, sizeof views);
  int32_t nfl = 23, nbi = 0, noff = 0;
  memcpy(views, &nfl, 4);
  memcpy(views + 4, vdata, 4);
  memcpy(views + 8, &nbi, 4);
  memcpy(views + 12, &noff, 4);
  const void *vbufs[4] = { NULL, views, vdata, vsizes };
  memset(&aa, 0, sizeof aa);
  aa.length = 1;
  aa.n_buffers = 4;
  aa.buffers = vbufs;
  assert(mizu_arrow_str_at(&aa, 0, 3, &sp, &sl) == 1);
  assert(sl == 23 && sp == (const uint8_t *) vdata);
  /* a span past the declared size rejects */
  noff = 40;
  memcpy(views + 12, &noff, 4);
  assert(mizu_arrow_str_at(&aa, 0, 3, &sp, &sl) == -1);
  /* a buffer index past the array rejects */
  noff = 0;
  nbi = 2;
  memcpy(views + 8, &nbi, 4);
  memcpy(views + 12, &noff, 4);
  assert(mizu_arrow_str_at(&aa, 0, 3, &sp, &sl) == -1);
  /* the trailing slot referenced: the no-sizes tell — crosses unbounded */
  nbi = 1;
  memcpy(views + 8, &nbi, 4);
  assert(mizu_arrow_str_at(&aa, 0, 3, &sp, &sl) == 1);
  assert(sp == vsizes && sl == 23);
  /* the no-sizes layout proper: [NULL, views, data] — previously declined */
  const void *nbuf3[3] = { NULL, views, vdata };
  nbi = 0;
  memcpy(views + 8, &nbi, 4);
  aa.n_buffers = 3;
  aa.buffers = nbuf3;
  assert(mizu_arrow_str_at(&aa, 0, 3, &sp, &sl) == 1);
  assert(sl == 23 && sp == (const uint8_t *) vdata);
  nbi = 1;
  memcpy(views + 8, &nbi, 4);
  assert(mizu_arrow_str_at(&aa, 0, 3, &sp, &sl) == -1);

  printf("test_wire: OK\n");
  return 0;
}
