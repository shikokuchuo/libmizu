/* Unit tier: the spill free list, lent-region ledger, zc producer-loan
   release, retain table, open cache, and the staging services — exercised
   in-process over a bare handle (no transport). Assert-based; run via
   `make test`. */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "internal.h"

/* A handle on the stack: the spill services need only the base members. */
static mizu_handle h;
static int drops;
static void *last_drop;

static void count_drop(void *ctx, void *pin) {
  (void) ctx;
  drops++;
  last_drop = pin;
}

static void handle_reset(void) {
  memset(&h, 0, sizeof(h));
  h.binding.drop = count_drop;
  drops = 0;
  last_drop = NULL;
}

int main(void) {
  handle_reset();

  /* the collect-side keeperless gate: the immediate kinds and the
     self-contained codec magics skip the keeper-drop wake; kinds that
     carry keepers and reference-capable streams never qualify */
  const unsigned char r_codec[1] = { MIZU_CODEC_MAGIC };
  const unsigned char p_codec[1] = { MIZU_PYMIZU_CODEC_MAGIC };
  const unsigned char pickle[1] = { 0x80 };
  assert(mizu_keeperless(MIZU_KIND_NIL, NULL));
  assert(mizu_keeperless(MIZU_KIND_RAWVEC, NULL));
  assert(mizu_keeperless(MIZU_KIND_STR1, NULL));
  assert(mizu_keeperless(MIZU_KIND_INLINE, r_codec));
  assert(mizu_keeperless(MIZU_KIND_INLINE, p_codec));
  assert(!mizu_keeperless(MIZU_KIND_INLINE, pickle));
  assert(!mizu_keeperless(MIZU_KIND_INLINE, (const unsigned char *) "B"));
  assert(!mizu_keeperless(MIZU_KIND_INLINE, (const unsigned char *) "X"));
  assert(!mizu_keeperless(MIZU_KIND_SHM_RAW, r_codec));
  assert(!mizu_keeperless(MIZU_KIND_SHM_VEC, r_codec));
  assert(!mizu_keeperless(MIZU_KIND_RAWSPILL, r_codec));

  /* checkout: fresh create at the pow2 size class, recorded in staging */
  mizu_shm *a = mizu_spill_region_get(&h.fl, 100);
  assert(a != NULL && a->size == MIZU_SPILL_FL_FLOOR);
  assert(h.fl.staging == a && !h.fl.last_reused);

  /* rollback returns an uncommitted checkout to the free list */
  mizu_stage_rollback(&h);
  assert(h.fl.staging == NULL && h.fl.n == 1);

  /* the next checkout pops it (smallest fitting) */
  mizu_shm *b = mizu_spill_region_get(&h.fl, 100);
  assert(b == a && h.fl.last_reused && h.fl.hits == 1 && h.fl.n == 0);

  /* commit a SPILL retain into slot 3, then release at consumer-done:
     the region surrenders to the free list */
  mizu_keeper tab[8];
  memset(tab, 0, sizeof(tab));
  mizu_stage_retain(&h, b);
  mizu_keeper_commit(&h, tab, 3);
  assert(h.fl.staging == NULL && h.fl.staging_kind == MIZU_KEEP_FREE);
  assert(tab[3].kind == MIZU_KEEP_SPILL && tab[3].region == b &&
         tab[3].key == -1);
  mizu_keeper_release(&h, tab, 3);
  assert(tab[3].kind == MIZU_KEEP_FREE && h.fl.n == 1);

  /* a pin rides the entry and drops through the hook at release */
  int token = 42;
  mizu_shm *c = mizu_spill_region_get(&h.fl, 100);
  mizu_stage_retain(&h, c);
  mizu_stage_pin(&h, &token);
  assert(h.fl.staging_kind == MIZU_KEEP_SPILL);   /* pin doesn't rekindle */
  mizu_keeper_commit(&h, tab, 4);
  assert(tab[4].pin == &token);
  mizu_keeper_release(&h, tab, 4);
  assert(drops == 1 && last_drop == &token);

  /* pin-only staging (the serialize tiers): no region, kind PIN */
  mizu_stage_pin(&h, &token);
  assert(h.fl.staging_kind == MIZU_KEEP_PIN && h.fl.staging == NULL);
  mizu_keeper_commit(&h, tab, 5);
  mizu_keeper_release(&h, tab, 5);
  assert(drops == 2);

  /* rollback drops a registered pin */
  mizu_stage_pin(&h, &token);
  mizu_stage_rollback(&h);
  assert(drops == 3 && h.fl.staging_pin == NULL);

  /* discard (the never-published path): region + pin both released */
  mizu_shm *d = mizu_spill_region_get(&h.fl, 100);
  mizu_stage_retain(&h, d);
  mizu_stage_pin(&h, &token);
  mizu_keeper_discard(&h);
  assert(drops == 4 && h.fl.n == 1 && h.fl.staging == NULL);

  /* ZC: the producer-loan store, release to the ledger while a view is
     outstanding, sweep back to the free list once it releases */
  mizu_shm *z = mizu_spill_region_get(&h.fl, 100);
  mizu_stage_retain_zc(&h, z);
  assert(mizu_zc_refcount(z) == 1 && mizu_zc_flags(z) == 0);
  mizu_keeper_commit(&h, tab, 6);
  mizu_zc_ref(z);                       /* the consumer's view wrap */
  assert(mizu_zc_refcount(z) == 2);
  mizu_keeper_release(&h, tab, 6);      /* consumer-done: loan drops */
  assert(h.fl.led_n == 1 && h.fl.n == 0);   /* views outstanding: lent */
  mizu_ledger_sweep(&h.fl, MIZU_LEDGER_MAX);
  assert(h.fl.led_n == 1);             /* count still 1: stays */
  mizu_zc_unref(z);                     /* the view releases */
  mizu_ledger_sweep(&h.fl, MIZU_LEDGER_MAX);
  assert(h.fl.led_n == 0 && h.fl.n == 1);   /* back to the free list */

  /* ZC with no view outstanding: straight to the free list (z was
     popped for this checkout, so only z2 returns) */
  mizu_shm *z2 = mizu_spill_region_get(&h.fl, 100);
  mizu_stage_retain_zc(&h, z2);
  mizu_keeper_commit(&h, tab, 7);
  mizu_keeper_release(&h, tab, 7);
  assert(h.fl.n == 1 && h.fl.led_n == 0);

  /* the death backstop: unflagged lent regions rejoin; REFHELD leak +
     unlink (never force-reclaimed) */
  mizu_shm *z3 = mizu_spill_region_get(&h.fl, 100);
  mizu_stage_retain_zc(&h, z3);
  mizu_keeper_commit(&h, tab, 0);
  mizu_zc_ref(z3);
  mizu_keeper_release(&h, tab, 0);
  mizu_shm *z4 = mizu_spill_region_get(&h.fl, 100);
  mizu_stage_retain_zc(&h, z4);
  mizu_keeper_commit(&h, tab, 1);
  mizu_zc_ref(z4);
  mizu_zc_flag_refheld(z4);
  mizu_keeper_release(&h, tab, 1);
  assert(h.fl.led_n == 2);
  uint32_t before = h.fl.n;
  mizu_ledger_force(&h.fl, -1);
  assert(h.fl.led_n == 0);
  assert(h.fl.n == before + 1);        /* z3 rejoined; z4 leaked + unlinked
                                          (destroyed — not observable) */

  /* keyed force touches only that consumer's entries */
  mizu_shm *z5 = mizu_spill_region_get(&h.fl, 100);
  mizu_stage_retain_zc(&h, z5);
  mizu_keeper_commit(&h, tab, 2);
  mizu_zc_ref(z5);
  tab[2].key = 7;                      /* the pool's release-point keying */
  mizu_keeper_release(&h, tab, 2);
  assert(h.fl.led_n == 1);
  mizu_ledger_force(&h.fl, 3);          /* a different consumer: untouched */
  assert(h.fl.led_n == 1);
  mizu_ledger_force(&h.fl, 7);
  assert(h.fl.led_n == 0);

  /* free-list caps: a region past the byte cap is destroyed, not listed */
  mizu_shm *big = NULL;
  assert(mizu_shm_create_heap(&big, MIZU_SPILL_FL_BYTES + 4096) ==
         MIZU_ERRCAT_NONE);
  uint32_t n_before = h.fl.n;
  mizu_spill_fl_insert(&h.fl, big);
  assert(h.fl.n == n_before);

  /* open cache: store, hit, LRU eviction at capacity, teardown. The
     cache owns its mappings (evict/teardown closes them), so store
     consumer-opened mappings and keep the creator handles apart. */
  mizu_open_cache oc;
  memset(&oc, 0, sizeof(oc));
  mizu_shm *keep[MIZU_OPEN_CACHE_MAX + 1];
  char names[MIZU_OPEN_CACHE_MAX + 1][MIZU_NAME_MAX];
  for (int i = 0; i <= MIZU_OPEN_CACHE_MAX; i++) {
    assert(mizu_shm_create_heap(&keep[i], 4096) == MIZU_ERRCAT_NONE);
    snprintf(names[i], sizeof(names[i]), "%s", keep[i]->name);
    mizu_shm *view = mizu_shm_open_ro_heap(names[i]);
    assert(view != NULL);
    mizu_oc_store(&oc, view);
  }
  assert(oc.misses == MIZU_OPEN_CACHE_MAX + 1);
  /* the first entry was evicted (LRU); the rest hit */
  assert(mizu_oc_lookup(&oc, (const unsigned char *) names[0],
                       (uint32_t) strlen(names[0])) == NULL);
  for (int i = 1; i <= MIZU_OPEN_CACHE_MAX; i++)
    assert(mizu_oc_lookup(&oc, (const unsigned char *) names[i],
                         (uint32_t) strlen(names[i])) != NULL);
  mizu_oc_teardown(&oc);
  assert(oc.maps[1] == NULL);
  /* the cache closed its mappings; the creators still own the names */
  for (int i = 0; i <= MIZU_OPEN_CACHE_MAX; i++)
    mizu_shm_close(keep[i], 1);

  /* mizu_read_region: a hit through the cache, and the gone verdict */
  mizu_spill_fl_teardown(&h.fl);        /* retire the regions held so far */
  handle_reset();
  mizu_shm *target = NULL;
  assert(mizu_shm_create_heap(&target, 4096) == MIZU_ERRCAT_NONE);
  memset(target->addr, 0x7E, 64);
  mizu_read_ctx ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.size = sizeof(ctx);
  ctx.handle = &h;
  mizu_shm *got = mizu_read_region(&ctx, (const uint8_t *) target->name,
                                 target->name_len);
  assert(got != NULL && !ctx.gone);
  assert(((unsigned char *) got->addr)[0] == 0x7E);
  assert(h.oc.hits == 0 && h.oc.misses == 1);
  got = mizu_read_region(&ctx, (const uint8_t *) target->name,
                        target->name_len);
  assert(got != NULL && h.oc.hits == 1);
  mizu_shm *gone = mizu_read_region(&ctx, (const uint8_t *) "/mizu_0_0", 8);
  assert(gone == NULL && ctx.gone == 1);
  mizu_shm_close(target, 1);
  mizu_oc_teardown(&h.oc);

  /* handle teardown: everything closes, every pin drops */
  handle_reset();
  mizu_shm *t1 = mizu_spill_region_get(&h.fl, 100);
  mizu_stage_retain(&h, t1);
  mizu_stage_pin(&h, &token);
  mizu_keeper_commit(&h, tab, 0);
  mizu_shm *t2 = mizu_spill_region_get(&h.fl, 100);
  mizu_stage_retain(&h, t2);
  mizu_keeper_commit(&h, tab, 1);
  mizu_keeper_release(&h, tab, 1);      /* one in the free list */
  mizu_keepers_teardown(&h, tab, 8);
  assert(drops == 1);
  mizu_spill_fl_teardown(&h.fl);
  assert(h.fl.n == 0 && h.fl.total == 0 && h.fl.staging == NULL);

  puts("test_spill: ok");
  return 0;
}
