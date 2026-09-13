/* Unit tier: the dual-form wire helpers (rei_ext.h) — timeout conversion
   edges, the NA stores, the aux pack identities, and the REIH header
   write/check round trip with its rejection cases. Assert-based; run via
   `make test`. */

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "rei.h"
#include "rei_ext.h"

int main(void) {
  /* rei_timeout_ms: non-finite waits indefinitely, <= 0 polls. */
  assert(rei_timeout_ms(NAN) == -1);
  assert(rei_timeout_ms(INFINITY) == -1);
  assert(rei_timeout_ms(-INFINITY) == -1);
  assert(rei_timeout_ms(0) == 0);
  assert(rei_timeout_ms(-2.5) == 0);
  assert(rei_timeout_ms(1) == 1000);
  assert(rei_timeout_ms(0.001) == 1);
  assert(rei_timeout_ms(1e-6) == 0.001);   /* sub-ms stays fractional */

  /* The NA stores and constants. */
  assert(REI_NA_INT32 == INT32_MIN);
  uint64_t na64 = REI_NA_INT64;
  assert(na64 == (uint64_t) INT64_MIN);
  unsigned char buf[8];
  rei_store_na_real(buf);
  uint64_t bits;
  memcpy(&bits, buf, 8);
  assert(bits == REI_NA_REAL_BITS);
  double na_real;
  memcpy(&na_real, buf, 8);
  assert(isnan(na_real));

  /* aux pack/unpack identities (rei.h's kind table). */
  assert(rei_aux_type(rei_aux_rawspill_pool(REI_TYPE_INT64, 27)) == 32);
  assert(rei_aux_hi(rei_aux_rawspill_pool(REI_TYPE_INT64, 27)) == 27);
  assert(rei_aux_rawspill_pool(REI_TYPE_REAL, 255) ==
         ((uint64_t) REI_TYPE_REAL | ((uint64_t) 255 << 8)));
  assert(rei_aux_type(rei_aux_shm_vec(REI_TYPE_STR, 65536 + 64)) == 16);
  assert(rei_aux_hi(rei_aux_shm_vec(REI_TYPE_STR, 65536 + 64)) == 65600);

  /* REIH round trip. */
  unsigned char reih[256];
  memset(reih, 0xAA, sizeof reih);
  rei_reih_write(reih, REI_TYPE_INT, 42);
  int type = 0;
  int64_t n = 0;
  assert(rei_reih_check(reih, sizeof reih, &type, &n) == 0);
  assert(type == REI_TYPE_INT && n == 42);
  for (size_t i = 24; i < REI_HEADER_SIZE; i++)
    assert(reih[i] == 0);   /* the reserved band, zc words included */

  /* Rejections: size, magic, type, extent. */
  assert(rei_reih_check(reih, REI_HEADER_SIZE - 1, &type, &n) == -1);
  unsigned char bad[256];
  memcpy(bad, reih, sizeof bad);
  bad[0] ^= 0xFF;   /* magic */
  assert(rei_reih_check(bad, sizeof bad, &type, &n) == -1);
  memcpy(bad, reih, sizeof bad);
  bad[4] = 7;       /* no such atomic wire type */
  assert(rei_reih_check(bad, sizeof bad, &type, &n) == -1);
  memcpy(bad, reih, sizeof bad);
  rei_reih_write(bad, REI_TYPE_REAL, 1 << 20);   /* 8 MB into 128 B */
  assert(rei_reih_check(bad, sizeof bad, &type, &n) == -1);
  memcpy(bad, reih, sizeof bad);
  bad[16] = 0xFF;   /* attrs_size past the region */
  assert(rei_reih_check(bad, sizeof bad, &type, &n) == -1);
  /* a negative element count rejects */
  memcpy(bad, reih, sizeof bad);
  int64_t neg = -1;
  memcpy(bad + 8, &neg, 8);
  assert(rei_reih_check(bad, sizeof bad, &type, &n) == -1);

  printf("test_wire: OK\n");
  return 0;
}
