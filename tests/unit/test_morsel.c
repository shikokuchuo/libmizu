/* Unit tier: the mizu_morsel_* map morsel protocol — header layout and
   validation, the generation-fenced claim, AIMD batch sizing, cursor
   issue and exhaustion, reset re-arm, the abandon trim, cancel, and the
   lost-set scan. Two in-process runners on one region stand in for the
   bindings' workers. Assert-based; run via `make test`. */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "mizu_ext.h"

#define TEST_MAGIC 0x534F524Du   /* "SORM" (mizu's tag; pymizu keeps PYRM) */

static mizu_shm *map_region(mizu_morsel_hdr *h, uint64_t n, uint64_t msz,
                           uint32_t x_type, uint32_t out_type,
                           uint64_t out_m) {
  uint64_t x_len = x_type != 0 ? n * mizu_type_elt_size((int) x_type) : 0;
  uint64_t size = mizu_morsel_layout(h, TEST_MAGIC, n, msz, 100, x_type,
                                    x_len, out_type, out_m, MIZU_MAX_WORKERS);
  assert(size != 0);
  mizu_shm *shm;
  assert(mizu_shm_create(&shm, (size_t) size) == MIZU_OK);
  memcpy(shm->addr, h, sizeof *h);
  return shm;
}

static void layout_tests(void) {
  mizu_morsel_hdr h;
  /* n = 1000, msz = 10, desc 100, x REAL (8000), out REAL x2 */
  uint64_t size = mizu_morsel_layout(&h, TEST_MAGIC, 1000, 10, 100,
                                    MIZU_TYPE_REAL, 8000, MIZU_TYPE_REAL, 2,
                                    MIZU_MAX_WORKERS);
  assert(size != 0);
  assert(h.magic == TEST_MAGIC && h.version == MIZU_ABI_VERSION);
  assert(h.desc_off == 128 && h.desc_len == 100);
  assert(h.x_kind == MIZU_MORSEL_X_RAW && h.x_type == MIZU_TYPE_REAL);
  assert(h.x_off == 256 && h.x_len == 8000);
  assert(h.state_off == 8256 && (h.state_off & 63) == 0);
  assert(h.out_off == 8640 && h.out_m == 2 && h.out_elt == 8);
  assert(h.morsel_size == 10 && h.n_morsels == 100);
  assert(h.claim_n == MIZU_MAX_WORKERS);
  assert(size == 8640 + 1000 * 2 * 8);

  /* no optional sections */
  size = mizu_morsel_layout(&h, TEST_MAGIC, 7, 3, 4, 0, 0, 0, 0,
                           MIZU_MAX_WORKERS);
  assert(size != 0);
  assert(h.x_kind == MIZU_MORSEL_X_DESC && h.x_off == 0 && h.x_len == 0);
  assert(h.out_type == 0 && h.out_off == 0 && h.out_m == 0);
  assert(h.desc_off == 128 && h.state_off == 192);
  assert(size == 192 + MIZU_MORSEL_CLAIM_OFF + MIZU_MAX_WORKERS * 4);
  assert(h.n_morsels == 3);

  /* rejections */
  assert(mizu_morsel_layout(&h, TEST_MAGIC, 0, 1, 1, 0, 0, 0, 0, 64) == 0);
  assert(mizu_morsel_layout(&h, TEST_MAGIC, 10, 0, 1, 0, 0, 0, 0, 64) == 0);
  assert(mizu_morsel_layout(&h, TEST_MAGIC, 10, 11, 1, 0, 0, 0, 0, 64) == 0);
  assert(mizu_morsel_layout(&h, TEST_MAGIC, 10, 1, 0, 0, 0, 0, 0, 64) == 0);
  assert(mizu_morsel_layout(&h, TEST_MAGIC, 10, 1, 1, 0, 0, 0, 0, 0) == 0);
  /* x_len must be exactly n elements */
  assert(mizu_morsel_layout(&h, TEST_MAGIC, 10, 1, 1, MIZU_TYPE_REAL, 79, 0,
                           0, 64) == 0);
  /* overflow guards */
  assert(mizu_morsel_layout(&h, TEST_MAGIC, (uint64_t) 1 << 46, 1, 1, 0, 0,
                           MIZU_TYPE_REAL, 1, 64) == 0);
}

static void check_tests(void) {
  mizu_morsel_hdr h;
  mizu_shm *shm = map_region(&h, 1000, 10, MIZU_TYPE_REAL, MIZU_TYPE_REAL, 2);
  mizu_morsel_hdr v;
  assert(mizu_morsel_hdr_check(shm->addr, shm->size, TEST_MAGIC, &v) == NULL);
  assert(v.n == 1000 && v.out_m == 2 && v.state_off == h.state_off);

  /* too small */
  assert(mizu_morsel_hdr_check(shm->addr, 127, TEST_MAGIC, NULL) != NULL);
  /* wrong magic */
  assert(mizu_morsel_hdr_check(shm->addr, shm->size, 0x4D525950u,
                              NULL) != NULL);

  mizu_morsel_hdr bad = h;
  bad.version += 1;
  memcpy(shm->addr, &bad, sizeof bad);
  assert(mizu_morsel_hdr_check(shm->addr, shm->size, TEST_MAGIC, NULL) !=
         NULL);
  bad = h;
  bad.n_morsels += 1;
  memcpy(shm->addr, &bad, sizeof bad);
  assert(mizu_morsel_hdr_check(shm->addr, shm->size, TEST_MAGIC, NULL) !=
         NULL);
  bad = h;
  bad.x_off += 64;   /* placement is pinned */
  memcpy(shm->addr, &bad, sizeof bad);
  assert(mizu_morsel_hdr_check(shm->addr, shm->size, TEST_MAGIC, NULL) !=
         NULL);
  bad = h;
  bad.out_off += 64;
  memcpy(shm->addr, &bad, sizeof bad);
  assert(mizu_morsel_hdr_check(shm->addr, shm->size, TEST_MAGIC, NULL) !=
         NULL);
  bad = h;
  bad.state_off = h.state_off + 64;
  memcpy(shm->addr, &bad, sizeof bad);
  assert(mizu_morsel_hdr_check(shm->addr, shm->size, TEST_MAGIC, NULL) !=
         NULL);
  memcpy(shm->addr, &h, sizeof h);   /* restore */
  mizu_shm_close(shm, 1);
}

static void claim_tests(void) {
  mizu_morsel_hdr h;
  mizu_shm *shm = map_region(&h, 1000, 1, 0, 0, 0);
  void *base = shm->addr;
  mizu_morsel_sizer s0, s1;
  mizu_morsel_sizer_init(&s0);
  mizu_morsel_sizer_init(&s1);
  uint64_t m, k;
  int help;

  /* first claims: k ramps from 1 */
  assert(mizu_morsel_next(base, &h, &s0, 0, 0, NULL, 0, 1.0, &m, &k,
                         &help) == 1);
  assert(m == 0 && k == 1 && help == 0);
  assert(mizu_morsel_next(base, &h, &s1, 1, 0, NULL, 0, 1.0, &m, &k,
                         &help) == 1);
  assert(m == 1 && k == 1);

  /* instant batches hit the 2x growth clamp exactly: 1 -> 2 -> 4 ... */
  double now = 1.0;
  for (uint64_t want = 2; want <= 64; want *= 2) {
    assert(mizu_morsel_next(base, &h, &s0, 0, 0, NULL, 0, now, &m, &k,
                           &help) == 1);
    assert(k == want);
  }
  /* ... and clamp at the cap */
  assert(mizu_morsel_next(base, &h, &s0, 0, 0, NULL, 0, now, &m, &k,
                         &help) == 1);
  assert(k == 64);

  /* a slow interval shrinks immediately to the floor */
  now += 1.0;   /* 1 s for the last 64 morsels */
  assert(mizu_morsel_next(base, &h, &s0, 0, 0, NULL, 0, now, &m, &k,
                         &help) == 1);
  assert(k == 1);

  /* the generation fence: a straggler from run 1 claims nothing */
  assert(mizu_morsel_next(base, &h, &s1, 2, 1, NULL, 0, now, &m, &k,
                         &help) == 0);

  /* claim words read back as (gen << 2) | state */
  assert(mizu_morsel_claim(base, &h, 0) ==
         ((0u << 2) | MIZU_MORSEL_RUNNING));
  assert(mizu_morsel_claim(base, &h, 3) == ((0u << 2) | MIZU_MORSEL_IDLE));

  /* pinned k bypasses the policy */
  assert(mizu_morsel_next(base, &h, &s1, 1, 0, NULL, 7, now, &m, &k,
                         &help) == 1);
  assert(k == 7);

  /* cancel stops peers */
  mizu_morsel_cancel_set(base, &h);
  assert(mizu_morsel_cancel_get(base, &h) == 1);
  assert(mizu_morsel_next(base, &h, &s1, 1, 0, NULL, 0, now, &m, &k,
                         &help) == 0);

  /* reset re-arms: generation 1, cursor zeroed, cancel cleared */
  assert(mizu_morsel_reset(base, &h) == 1);
  assert(mizu_morsel_generation(base, &h) == 1);
  assert(mizu_morsel_cursor(base, &h) == 0);
  assert(mizu_morsel_cancel_get(base, &h) == 0);
  assert(mizu_morsel_claim(base, &h, 0) == ((1u << 2) | MIZU_MORSEL_IDLE));
  /* the run boundary re-ramps the sizer */
  assert(mizu_morsel_next(base, &h, &s0, 0, 1, NULL, 0, 2.0, &m, &k,
                         &help) == 1);
  assert(m == 0 && k == 1 && s0.run_gen == 1);
  /* the prior run's straggler fails its first-call CAS against the
     re-armed word */
  assert(mizu_morsel_next(base, &h, &s1, 1, 0, NULL, 0, 2.0, &m, &k,
                         &help) == 0);

  mizu_shm_close(shm, 1);
}

static void exhaust_and_abandon_tests(void) {
  mizu_morsel_hdr h;
  mizu_shm *shm = map_region(&h, 10, 1, 0, 0, 0);
  void *base = shm->addr;
  mizu_morsel_sizer sz;
  mizu_morsel_sizer_init(&sz);
  uint64_t m, k;
  int help;

  /* pin k = 3: 3, 3, 3, then the final partial grant of 1, then stop */
  assert(mizu_morsel_next(base, &h, &sz, 0, 0, NULL, 3, 1.0, &m, &k,
                         &help) == 1 && m == 0 && k == 3);
  assert(mizu_morsel_next(base, &h, &sz, 0, 0, NULL, 3, 1.0, &m, &k,
                         &help) == 1 && m == 3 && k == 3);
  assert(mizu_morsel_next(base, &h, &sz, 0, 0, NULL, 3, 1.0, &m, &k,
                         &help) == 1 && m == 6 && k == 3);
  assert(mizu_morsel_next(base, &h, &sz, 0, 0, NULL, 3, 1.0, &m, &k,
                         &help) == 1 && m == 9 && k == 1);
  assert(mizu_morsel_next(base, &h, &sz, 0, 0, NULL, 3, 1.0, &m, &k,
                         &help) == 0);
  /* the cursor overshoots but reads back clamped */
  assert(mizu_morsel_cursor(base, &h) == 10);

  /* the trim is armed by exhaustion: an IDLE lane folds to ABANDONED */
  assert(mizu_morsel_abandon(base, &h, 1, 0) == MIZU_MORSEL_ABANDONED);
  assert(mizu_morsel_claim(base, &h, 1) == ((0u << 2) | MIZU_MORSEL_ABANDONED));
  /* a trimmed lane never starts */
  mizu_morsel_sizer s2;
  mizu_morsel_sizer_init(&s2);
  assert(mizu_morsel_next(base, &h, &s2, 1, 0, NULL, 0, 1.0, &m, &k,
                         &help) == 0);
  /* RUNNING verdict for a live lane, IDLE when the trigger is unarmed */
  assert(mizu_morsel_abandon(base, &h, 0, 0) == MIZU_MORSEL_RUNNING);
  assert(mizu_morsel_reset(base, &h) == 1);
  assert(mizu_morsel_abandon(base, &h, 2, 1) == MIZU_MORSEL_IDLE);
  /* the wrong generation trims nothing */
  assert(mizu_morsel_next(base, &h, &s2, 2, 1, NULL, 0, 2.0, &m, &k,
                         &help) == 1);
  mizu_morsel_cancel_set(base, &h);
  assert(mizu_morsel_abandon(base, &h, 3, 0) == MIZU_MORSEL_IDLE);

  mizu_shm_close(shm, 1);
}

static void signal_tests(void) {
  mizu_morsel_hdr h;
  mizu_shm *shm = map_region(&h, 100, 1, 0, 0, 0);
  void *base = shm->addr;
  mizu_morsel_sizer sz;
  mizu_morsel_sizer_init(&sz);
  uint64_t m, k;
  int help;

  MIZU_ATOMIC(uint32_t) help_wanted = 0, shutdown = 0;
  MIZU_ATOMIC(int) owner_dead = 0;
  mizu_pool_sig sig = { &help_wanted, &shutdown, &owner_dead };

  /* help: flagged on the issue, and the interval skips the cost update */
  help_wanted = 1;
  assert(mizu_morsel_next(base, &h, &sz, 0, 0, &sig, 0, 1.0, &m, &k,
                         &help) == 1 && help == 1);
  assert(sz.skip == 1);
  double cost = sz.cost;
  help_wanted = 0;
  assert(mizu_morsel_next(base, &h, &sz, 0, 0, &sig, 0, 2.0, &m, &k,
                         &help) == 1 && help == 0);
  assert(sz.skip == 0 && sz.cost == cost);

  /* shutdown / owner death stop the runner */
  shutdown = 1;
  assert(mizu_morsel_next(base, &h, &sz, 0, 0, &sig, 0, 3.0, &m, &k,
                         &help) == 0);
  shutdown = 0;
  owner_dead = 1;
  assert(mizu_morsel_next(base, &h, &sz, 0, 0, &sig, 0, 3.0, &m, &k,
                         &help) == 0);

  mizu_shm_close(shm, 1);
}

static void lost_tests(void) {
  /* element-space spans, unsorted input */
  mizu_morsel_span spans[] = { {10, 20}, {0, 5}, {40, 50} };
  mizu_morsel_span out[4];
  size_t g = mizu_morsel_lost(spans, 3, 60, out);
  assert(g == 3);
  assert(out[0].lo == 5 && out[0].hi == 10);
  assert(out[1].lo == 20 && out[1].hi == 40);
  assert(out[2].lo == 50 && out[2].hi == 60);

  /* nothing collected: the whole issued range is lost */
  g = mizu_morsel_lost(spans, 0, 30, out);
  assert(g == 1 && out[0].lo == 0 && out[0].hi == 30);

  /* coverage past the bound clamps the scan */
  spans[0].lo = 0;
  spans[0].hi = 100;
  g = mizu_morsel_lost(spans, 1, 50, out);
  assert(g == 0);

  /* overlapping spans merge */
  spans[0].lo = 0;
  spans[0].hi = 10;
  spans[1].lo = 5;
  spans[1].hi = 15;
  g = mizu_morsel_lost(spans, 2, 20, out);
  assert(g == 1 && out[0].lo == 15 && out[0].hi == 20);

  /* a span starting past the bound emits the tail gap once */
  spans[0].lo = 100;
  spans[0].hi = 200;
  g = mizu_morsel_lost(spans, 1, 50, out);
  assert(g == 1 && out[0].lo == 0 && out[0].hi == 50);
}

static void span_tests(void) {
  mizu_morsel_hdr h;
  uint64_t size = mizu_morsel_layout(&h, TEST_MAGIC, 25, 10, 4, 0, 0, 0, 0,
                                    64);
  assert(size != 0);
  uint64_t lo, hi;
  mizu_morsel_span_of(&h, 0, 1, &lo, &hi);
  assert(lo == 0 && hi == 10);
  mizu_morsel_span_of(&h, 2, 1, &lo, &hi);   /* the final partial morsel */
  assert(lo == 20 && hi == 25);
  mizu_morsel_span_of(&h, 0, 3, &lo, &hi);
  assert(lo == 0 && hi == 25);
}

int main(void) {
  layout_tests();
  check_tests();
  claim_tests();
  exhaust_and_abandon_tests();
  signal_tests();
  lost_tests();
  span_tests();
  puts("test_morsel: ok");
  return 0;
}
