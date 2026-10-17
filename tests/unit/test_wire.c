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

  printf("test_wire: OK\n");
  return 0;
}
