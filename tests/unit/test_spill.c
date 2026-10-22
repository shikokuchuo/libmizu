/* Unit tier: the spill free list, lent-region ledger, zc producer-loan
   release, retain table, open cache, and the staging services — exercised
   in-process over a bare handle (no transport). Assert-based; run via
   `make test`. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __linux__
#include <sys/utsname.h>
#endif

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

  /* the collect-side keeperless gate: the immediate kinds and an INLINE
     frame stamped with the stager's keeperless claim skip the
     keeper-drop wake; the reference-capable kinds test keeper-ful even
     with the bit set (the claim is INLINE-only) */
  assert(mizu_keeperless(MIZU_KIND_NIL, 0));
  assert(mizu_keeperless(MIZU_KIND_RAWVEC, 0));
  assert(mizu_keeperless(MIZU_KIND_STR1, 0));
  assert(mizu_keeperless(MIZU_KIND_INLINE, MIZU_AUX_F_KEEPERLESS));
  assert(!mizu_keeperless(MIZU_KIND_INLINE, 0));
  assert(!mizu_keeperless(MIZU_KIND_SHM_RAW, MIZU_AUX_F_KEEPERLESS));
  assert(!mizu_keeperless(MIZU_KIND_SHM_VEC, MIZU_AUX_F_KEEPERLESS));
  assert(!mizu_keeperless(MIZU_KIND_RAWSPILL, MIZU_AUX_F_KEEPERLESS));

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
  assert(h.fl.lent_bytes == z->size);       /* counted on the ledger */
  mizu_ledger_sweep(&h.fl, MIZU_LEDGER_MAX);
  assert(h.fl.led_n == 1);             /* count still 1: stays */
  mizu_zc_unref(z);                     /* the view releases */
  mizu_ledger_sweep(&h.fl, MIZU_LEDGER_MAX);
  assert(h.fl.led_n == 0 && h.fl.n == 1);   /* back to the free list */
  assert(h.fl.lent_bytes == 0);

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
  mizu_ledger_force(&h.fl, -1, -1);
  assert(h.fl.led_n == 0);
  assert(h.fl.n == before + 1);        /* z3 rejoined; z4 leaked + unlinked
                                          (destroyed — not observable) */

  /* keyed force touches only that consumer's entries — kind-tagged, so
     a same-numbered slot of the other namespace never matches */
  mizu_shm *z5 = mizu_spill_region_get(&h.fl, 100);
  mizu_stage_retain_zc(&h, z5);
  mizu_keeper_commit(&h, tab, 2);
  mizu_zc_ref(z5);
  tab[2].key = MIZU_LED_KEY(MIZU_LED_WK, 7);   /* the pool's keying */
  mizu_keeper_release(&h, tab, 2);
  assert(h.fl.led_n == 1);
  mizu_ledger_force(&h.fl, MIZU_LED_WK, 3);    /* a different worker */
  assert(h.fl.led_n == 1);
  mizu_ledger_force(&h.fl, MIZU_LED_SUB, 7);   /* submitter namespace */
  assert(h.fl.led_n == 1);
  mizu_ledger_force(&h.fl, MIZU_LED_WK, 7);
  assert(h.fl.led_n == 0);

  /* the quota sweep rotates its window across calls: long-held regions
     at the ledger front must not starve the entries behind them */
  mizu_shm *r[4];
  for (int i = 0; i < 4; i++) {
    r[i] = mizu_spill_region_get(&h.fl, 100);
    mizu_stage_retain_zc(&h, r[i]);
    mizu_keeper_commit(&h, tab, (uint32_t) i);
    mizu_zc_ref(r[i]);                      /* held: stays lent */
    mizu_keeper_release(&h, tab, (uint32_t) i);
  }
  assert(h.fl.led_n == 4);
  uint32_t n0 = h.fl.n;
  mizu_zc_unref(r[2]);                      /* releasable */
  mizu_zc_unref(r[3]);
  mizu_ledger_sweep(&h.fl, 2);              /* visits r[0], r[1]: held */
  assert(h.fl.led_n == 4 && h.fl.n == n0);
  mizu_ledger_sweep(&h.fl, 2);              /* wraps to r[2], r[3] */
  assert(h.fl.led_n == 2 && h.fl.n == n0 + 2);

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
  mizu_spill_fl_teardown(&h, &h.fl);        /* retire the regions held so far */
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
  /* the wire-name grammar gate: a traversal or malformed name is refused
     before the open, reported gone like a vanished region */
  const char *bad_names[] = {
    "/mizu_../x", "/mizu_1", "/mizu_1_2x", "/mizu__1", "/etc/mizu_1_2"
  };
  for (size_t i = 0; i < sizeof bad_names / sizeof *bad_names; i++) {
    ctx.gone = 0;
    assert(mizu_read_region(&ctx, (const uint8_t *) bad_names[i],
                           (uint32_t) strlen(bad_names[i])) == NULL);
    assert(ctx.gone == 1);
  }
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
  mizu_spill_fl_teardown(&h, &h.fl);
  assert(h.fl.n == 0 && h.fl.total == 0 && h.fl.staging == NULL);

  /* free-list caps: sixteen entries per size class — the seventeenth is
     destroyed, not listed */
  handle_reset();
  for (int i = 0; i < 16; i++) {
    mizu_shm *q = NULL;
    assert(mizu_shm_create_heap(&q, 4096) == MIZU_ERRCAT_NONE);
    mizu_spill_fl_insert(&h.fl, q);
  }
  assert(h.fl.n == 16);
  mizu_shm *q3 = NULL;
  assert(mizu_shm_create_heap(&q3, 4096) == MIZU_ERRCAT_NONE);
  mizu_spill_fl_insert(&h.fl, q3);
  assert(h.fl.n == 16);              /* class full: destroyed */
  mizu_spill_fl_teardown(&h, &h.fl);

  /* the byte cap evicts largest-oldest until the insert fits */
  handle_reset();
  mizu_shm *e1 = NULL, *e2 = NULL, *e3 = NULL, *e4 = NULL;
  assert(mizu_shm_create_heap(&e1, (size_t) 16 << 20) == MIZU_ERRCAT_NONE);
  assert(mizu_shm_create_heap(&e2, (size_t) 16 << 20) == MIZU_ERRCAT_NONE);
  assert(mizu_shm_create_heap(&e3, (size_t) 16 << 20) == MIZU_ERRCAT_NONE);
  assert(mizu_shm_create_heap(&e4, (size_t) 16 << 20) == MIZU_ERRCAT_NONE);
  mizu_spill_fl_insert(&h.fl, e1);
  mizu_spill_fl_insert(&h.fl, e2);
  mizu_spill_fl_insert(&h.fl, e3);
  mizu_spill_fl_insert(&h.fl, e4);
  assert(h.fl.n == 4 && h.fl.total == ((size_t) 64 << 20));
  mizu_shm *e5 = NULL;
  assert(mizu_shm_create_heap(&e5, (size_t) 16 << 20) == MIZU_ERRCAT_NONE);
  mizu_spill_fl_insert(&h.fl, e5);   /* over the cap: e1 evicted */
  assert(h.fl.n == 4 && h.fl.total == ((size_t) 64 << 20));
  assert(h.fl.regions[0] == NULL ||
         h.fl.regions[0] != e1);      /* e1's slot was reused */
  mizu_spill_fl_teardown(&h, &h.fl);

  /* every slot occupied (four per class, eight classes): an insert of a
     fresh class destroys outright */
  handle_reset();
  for (int c = 0; c < 8; c++)
    for (int i = 0; i < 4; i++) {
      mizu_shm *o = NULL;
      assert(mizu_shm_create_heap(&o, (size_t) 4096 << c) ==
             MIZU_ERRCAT_NONE);
      mizu_spill_fl_insert(&h.fl, o);
    }
  assert(h.fl.n == MIZU_SPILL_FL_MAX);
  mizu_shm *o17 = NULL;
  assert(mizu_shm_create_heap(&o17, 1 << 20) == MIZU_ERRCAT_NONE);
  mizu_spill_fl_insert(&h.fl, o17);
  assert(h.fl.n == MIZU_SPILL_FL_MAX);   /* no room: destroyed */
  mizu_spill_fl_teardown(&h, &h.fl);

#ifdef __linux__
  /* the collapsed latch: a born-huge create (kernel >= 6.1) sets it, the
     insert's re-scan skip keeps it, and it rides the pop/reinsert cycle.
     Failure to collapse (older kernel) leaves it 0 — the insert's heal
     attempt then remains live, idempotently. */
  handle_reset();
  {
    struct utsname uts;
    int kv = uname(&uts) == 0
               ? atoi(uts.release) * 100 + atoi(strchr(uts.release, '.') + 1)
               : 0;
    int born_huge = kv >= 601;
    mizu_shm *g = NULL;
    assert(mizu_shm_create_heap(&g, MIZU_HUGE_EXTENT) == MIZU_ERRCAT_NONE);
    assert(g->collapsed == born_huge);
    mizu_spill_fl_insert(&h.fl, g);
    assert(g->collapsed == born_huge);
    mizu_shm *g2 = mizu_spill_region_get(&h.fl, MIZU_HUGE_EXTENT);
    assert(g2 == g && g2->collapsed == born_huge);
    mizu_stage_rollback(&h);   /* the checkout rejoins the free list */
    int found = 0;
    for (int i = 0; i < MIZU_SPILL_FL_MAX; i++)
      if (h.fl.regions[i] == g) found = 1;
    assert(h.fl.n == 1 && found);
    mizu_spill_fl_teardown(&h, &h.fl);
  }
#endif

  /* a free-list miss sweeps the ledger once and retries: a lent region
     whose last view released rejoins and serves the checkout */
  handle_reset();
  mizu_shm *lz = mizu_spill_region_get(&h.fl, 100);
  mizu_stage_retain_zc(&h, lz);
  mizu_keeper_commit(&h, tab, 0);
  mizu_zc_ref(lz);
  mizu_keeper_release(&h, tab, 0);    /* lent: rc 1 on the ledger */
  assert(h.fl.led_n == 1 && h.fl.n == 0);
  mizu_zc_unref(lz);                  /* the view releases */
  mizu_shm *lz2 = mizu_spill_region_get(&h.fl, 100);
  assert(lz2 == lz && h.fl.last_reused);   /* swept + popped */
  assert(h.fl.led_n == 0 && h.fl.n == 0);
  mizu_spill_fl_teardown(&h, &h.fl);

  /* the churn budget (a Linux-only signal): a miss with lent regions
     under the budget creates fresh and never engages the fallback; past
     the budget the same miss sets churn, and a reclaim that brings the
     total back under clears it. The past-budget state is
     field-simulated — 256 MB of real regions has no place in a unit
     test */
#ifdef __linux__
  handle_reset();
  mizu_shm *bz = mizu_spill_region_get(&h.fl, 100);
  mizu_stage_retain_zc(&h, bz);
  mizu_keeper_commit(&h, tab, 0);
  mizu_zc_ref(bz);
  mizu_keeper_release(&h, tab, 0);         /* lent: rc 1 on the ledger */
  assert(h.fl.led_n == 1 && h.fl.lent_bytes == bz->size);
  /* under the budget the same miss creates fresh: no churn */
  mizu_shm *bz2 = mizu_spill_region_get(&h.fl, 100);
  assert(bz2 != NULL && !h.fl.last_reused && h.fl.churn == 0);
  mizu_stage_rollback(&h);
  assert(h.fl.n == 1);
  mizu_shm *bz2r = mizu_spill_region_get(&h.fl, 100);
  assert(bz2r == bz2);                     /* the free-list gain pops it */
  mizu_stage_retain(&h, bz2);
  mizu_keeper_commit(&h, tab, 1);      /* checked out: the list is empty */
  /* past the budget the miss engages the fallback */
  h.fl.lent_bytes = MIZU_ZC_LENT_BUDGET + bz->size;
  mizu_shm *bz3 = mizu_spill_region_get(&h.fl, 100);
  assert(bz3 != NULL && !h.fl.last_reused && h.fl.churn == 1);
  mizu_stage_retain(&h, bz3);
  mizu_keeper_commit(&h, tab, 2);
  /* a sweep that reclaims nothing keeps it (the volume is unmoved) */
  mizu_ledger_sweep(&h.fl, MIZU_LEDGER_MAX);
  assert(h.fl.churn == 1 && h.fl.led_n == 1);
  /* a reclaim that brings the total back under clears it */
  mizu_zc_unref(bz);
  mizu_ledger_sweep(&h.fl, MIZU_LEDGER_MAX);
  assert(h.fl.led_n == 0 && h.fl.lent_bytes == MIZU_ZC_LENT_BUDGET);
  assert(h.fl.churn == 0 && h.fl.n == 1);
  mizu_keepers_teardown(&h, tab, 8);
  mizu_spill_fl_teardown(&h, &h.fl);
  assert(h.fl.lent_bytes == 0);
#endif

  /* a full ledger overflows: the region's mapping closes but its name
     is deferred to the teardown (a REF in flight can still resolve) */
  handle_reset();
  for (uint32_t i = 0; i < MIZU_LEDGER_MAX; i++) {
    mizu_shm *g = mizu_spill_region_get(&h.fl, 100);
    mizu_stage_retain_zc(&h, g);
    mizu_keeper_commit(&h, tab, 0);
    mizu_zc_ref(g);
    mizu_keeper_release(&h, tab, 0);
  }
  assert(h.fl.led_n == MIZU_LEDGER_MAX);
  mizu_shm *g64 = mizu_spill_region_get(&h.fl, 100);
  mizu_stage_retain_zc(&h, g64);
  mizu_keeper_commit(&h, tab, 0);
  mizu_zc_ref(g64);
  mizu_keeper_release(&h, tab, 0);
  assert(h.fl.led_n == MIZU_LEDGER_MAX);   /* overflowed, not ledgered */
  assert(h.fl.dropped_n == 1 && h.fl.dropped[0] == g64);
  /* the drop keeps its bytes counted (the file persists until
     teardown): the 64 ledger entries plus the dropped one */
  assert(h.fl.lent_bytes == (MIZU_LEDGER_MAX + 1) * MIZU_SPILL_FL_FLOOR);
  mizu_spill_fl_teardown(&h, &h.fl);           /* the deferred name dies here */
  assert(h.fl.dropped_n == 0 && h.fl.dropped == NULL);

  /* a zero-length region name is gone, never opened */
  handle_reset();
  mizu_read_ctx ctx2;
  memset(&ctx2, 0, sizeof(ctx2));
  ctx2.size = sizeof(ctx2);
  ctx2.handle = &h;
  assert(mizu_read_region(&ctx2, (const uint8_t *) "/mizu_1_1", 0) == NULL);
  assert(ctx2.gone == 1);

  puts("test_spill: ok");
  return 0;
}
