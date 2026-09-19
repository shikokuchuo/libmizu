/* Unit tier: the dual-form wire helpers (mizu_ext.h) — timeout conversion
   edges, the NA stores, the aux pack identities, and the MIZH header
   write/check round trip with its rejection cases. Assert-based; run via
   `make test`. */

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
  int64_t n = 0;
  assert(mizu_mizh_check(mizh, sizeof mizh, &type, &n) == 0);
  assert(type == MIZU_TYPE_INT && n == 42);
  for (size_t i = 24; i < MIZU_HEADER_SIZE; i++)
    assert(mizh[i] == 0);   /* the reserved band, zc words included */

  /* Rejections: size, magic, type, extent. */
  assert(mizu_mizh_check(mizh, MIZU_HEADER_SIZE - 1, &type, &n) == -1);
  unsigned char bad[256];
  memcpy(bad, mizh, sizeof bad);
  bad[0] ^= 0xFF;   /* magic */
  assert(mizu_mizh_check(bad, sizeof bad, &type, &n) == -1);
  memcpy(bad, mizh, sizeof bad);
  bad[4] = 7;       /* no such atomic wire type */
  assert(mizu_mizh_check(bad, sizeof bad, &type, &n) == -1);
  memcpy(bad, mizh, sizeof bad);
  mizu_mizh_write(bad, MIZU_TYPE_REAL, 1 << 20);   /* 8 MB into 128 B */
  assert(mizu_mizh_check(bad, sizeof bad, &type, &n) == -1);
  memcpy(bad, mizh, sizeof bad);
  bad[16] = 0xFF;   /* attrs_size past the region */
  assert(mizu_mizh_check(bad, sizeof bad, &type, &n) == -1);
  /* a negative element count rejects */
  memcpy(bad, mizh, sizeof bad);
  int64_t neg = -1;
  memcpy(bad + 8, &neg, 8);
  assert(mizu_mizh_check(bad, sizeof bad, &type, &n) == -1);

  printf("test_wire: OK\n");
  return 0;
}
