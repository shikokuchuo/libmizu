/* Unit tier: the spill free list, lent-region ledger, zc producer-loan
   release, retain table, open cache, and the staging services — exercised
   in-process over a bare handle (no transport). Assert-based; run via
   `make test`. */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "internal.h"

/* A handle on the stack: the spill services need only the base members. */
static rei_handle h;
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

  /* checkout: fresh create at the pow2 size class, recorded in staging */
  rei_shm *a = rei_spill_region_get(&h.fl, 100);
  assert(a != NULL && a->size == REI_SPILL_FL_FLOOR);
  assert(h.fl.staging == a && !h.fl.last_reused);

  /* rollback returns an uncommitted checkout to the free list */
  rei_stage_rollback(&h);
  assert(h.fl.staging == NULL && h.fl.n == 1);

  /* the next checkout pops it (smallest fitting) */
  rei_shm *b = rei_spill_region_get(&h.fl, 100);
  assert(b == a && h.fl.last_reused && h.fl.hits == 1 && h.fl.n == 0);

  /* commit a SPILL retain into slot 3, then release at consumer-done:
     the region surrenders to the free list */
  rei_keeper tab[8];
  memset(tab, 0, sizeof(tab));
  rei_stage_retain(&h, b);
  rei_keeper_commit(&h, tab, 3);
  assert(h.fl.staging == NULL && h.fl.staging_kind == REI_KEEP_FREE);
  assert(tab[3].kind == REI_KEEP_SPILL && tab[3].region == b &&
         tab[3].key == -1);
  rei_keeper_release(&h, tab, 3);
  assert(tab[3].kind == REI_KEEP_FREE && h.fl.n == 1);

  /* a pin rides the entry and drops through the hook at release */
  int token = 42;
  rei_shm *c = rei_spill_region_get(&h.fl, 100);
  rei_stage_retain(&h, c);
  rei_stage_pin(&h, &token);
  assert(h.fl.staging_kind == REI_KEEP_SPILL);   /* pin doesn't rekindle */
  rei_keeper_commit(&h, tab, 4);
  assert(tab[4].pin == &token);
  rei_keeper_release(&h, tab, 4);
  assert(drops == 1 && last_drop == &token);

  /* pin-only staging (the serialize tiers): no region, kind PIN */
  rei_stage_pin(&h, &token);
  assert(h.fl.staging_kind == REI_KEEP_PIN && h.fl.staging == NULL);
  rei_keeper_commit(&h, tab, 5);
  rei_keeper_release(&h, tab, 5);
  assert(drops == 2);

  /* rollback drops a registered pin */
  rei_stage_pin(&h, &token);
  rei_stage_rollback(&h);
  assert(drops == 3 && h.fl.staging_pin == NULL);

  /* discard (the never-published path): region + pin both released */
  rei_shm *d = rei_spill_region_get(&h.fl, 100);
  rei_stage_retain(&h, d);
  rei_stage_pin(&h, &token);
  rei_keeper_discard(&h);
  assert(drops == 4 && h.fl.n == 1 && h.fl.staging == NULL);

  /* ZC: the producer-loan store, release to the ledger while a view is
     outstanding, sweep back to the free list once it releases */
  rei_shm *z = rei_spill_region_get(&h.fl, 100);
  rei_stage_retain_zc(&h, z);
  assert(rei_zc_refcount(z) == 1 && rei_zc_flags(z) == 0);
  rei_keeper_commit(&h, tab, 6);
  rei_zc_ref(z);                       /* the consumer's view wrap */
  assert(rei_zc_refcount(z) == 2);
  rei_keeper_release(&h, tab, 6);      /* consumer-done: loan drops */
  assert(h.fl.led_n == 1 && h.fl.n == 0);   /* views outstanding: lent */
  rei_ledger_sweep(&h.fl, REI_LEDGER_MAX);
  assert(h.fl.led_n == 1);             /* count still 1: stays */
  rei_zc_unref(z);                     /* the view releases */
  rei_ledger_sweep(&h.fl, REI_LEDGER_MAX);
  assert(h.fl.led_n == 0 && h.fl.n == 1);   /* back to the free list */

  /* ZC with no view outstanding: straight to the free list (z was
     popped for this checkout, so only z2 returns) */
  rei_shm *z2 = rei_spill_region_get(&h.fl, 100);
  rei_stage_retain_zc(&h, z2);
  rei_keeper_commit(&h, tab, 7);
  rei_keeper_release(&h, tab, 7);
  assert(h.fl.n == 1 && h.fl.led_n == 0);

  /* the death backstop: unflagged lent regions rejoin; REFHELD leak +
     unlink (never force-reclaimed) */
  rei_shm *z3 = rei_spill_region_get(&h.fl, 100);
  rei_stage_retain_zc(&h, z3);
  rei_keeper_commit(&h, tab, 0);
  rei_zc_ref(z3);
  rei_keeper_release(&h, tab, 0);
  rei_shm *z4 = rei_spill_region_get(&h.fl, 100);
  rei_stage_retain_zc(&h, z4);
  rei_keeper_commit(&h, tab, 1);
  rei_zc_ref(z4);
  rei_zc_flag_refheld(z4);
  rei_keeper_release(&h, tab, 1);
  assert(h.fl.led_n == 2);
  uint32_t before = h.fl.n;
  rei_ledger_force(&h.fl, -1);
  assert(h.fl.led_n == 0);
  assert(h.fl.n == before + 1);        /* z3 rejoined; z4 leaked + unlinked
                                          (destroyed — not observable) */

  /* keyed force touches only that consumer's entries */
  rei_shm *z5 = rei_spill_region_get(&h.fl, 100);
  rei_stage_retain_zc(&h, z5);
  rei_keeper_commit(&h, tab, 2);
  rei_zc_ref(z5);
  tab[2].key = 7;                      /* the pool's release-point keying */
  rei_keeper_release(&h, tab, 2);
  assert(h.fl.led_n == 1);
  rei_ledger_force(&h.fl, 3);          /* a different consumer: untouched */
  assert(h.fl.led_n == 1);
  rei_ledger_force(&h.fl, 7);
  assert(h.fl.led_n == 0);

  /* free-list caps: a region past the byte cap is destroyed, not listed */
  rei_shm *big = NULL;
  assert(rei_shm_create_heap(&big, REI_SPILL_FL_BYTES + 4096) ==
         REI_ERRCAT_NONE);
  uint32_t n_before = h.fl.n;
  rei_spill_fl_insert(&h.fl, big);
  assert(h.fl.n == n_before);

  /* open cache: store, hit, LRU eviction at capacity, teardown. The
     cache owns its mappings (evict/teardown closes them), so store
     consumer-opened mappings and keep the creator handles apart. */
  rei_open_cache oc;
  memset(&oc, 0, sizeof(oc));
  rei_shm *keep[REI_OPEN_CACHE_MAX + 1];
  char names[REI_OPEN_CACHE_MAX + 1][REI_NAME_MAX];
  for (int i = 0; i <= REI_OPEN_CACHE_MAX; i++) {
    assert(rei_shm_create_heap(&keep[i], 4096) == REI_ERRCAT_NONE);
    snprintf(names[i], sizeof(names[i]), "%s", keep[i]->name);
    rei_shm *view = rei_shm_open_ro_heap(names[i]);
    assert(view != NULL);
    rei_oc_store(&oc, view);
  }
  assert(oc.misses == REI_OPEN_CACHE_MAX + 1);
  /* the first entry was evicted (LRU); the rest hit */
  assert(rei_oc_lookup(&oc, (const unsigned char *) names[0],
                       (uint32_t) strlen(names[0])) == NULL);
  for (int i = 1; i <= REI_OPEN_CACHE_MAX; i++)
    assert(rei_oc_lookup(&oc, (const unsigned char *) names[i],
                         (uint32_t) strlen(names[i])) != NULL);
  rei_oc_teardown(&oc);
  assert(oc.maps[1] == NULL);
  /* the cache closed its mappings; the creators still own the names */
  for (int i = 0; i <= REI_OPEN_CACHE_MAX; i++)
    rei_shm_close(keep[i], 1);

  /* rei_read_region: a hit through the cache, and the gone verdict */
  rei_spill_fl_teardown(&h.fl);        /* retire the regions held so far */
  handle_reset();
  rei_shm *target = NULL;
  assert(rei_shm_create_heap(&target, 4096) == REI_ERRCAT_NONE);
  memset(target->addr, 0x7E, 64);
  rei_read_ctx ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.size = sizeof(ctx);
  ctx.handle = &h;
  rei_shm *got = rei_read_region(&ctx, (const uint8_t *) target->name,
                                 target->name_len);
  assert(got != NULL && !ctx.gone);
  assert(((unsigned char *) got->addr)[0] == 0x7E);
  assert(h.oc.hits == 0 && h.oc.misses == 1);
  got = rei_read_region(&ctx, (const uint8_t *) target->name,
                        target->name_len);
  assert(got != NULL && h.oc.hits == 1);
  rei_shm *gone = rei_read_region(&ctx, (const uint8_t *) "/rei_0_0", 8);
  assert(gone == NULL && ctx.gone == 1);
  rei_shm_close(target, 1);
  rei_oc_teardown(&h.oc);

  /* handle teardown: everything closes, every pin drops */
  handle_reset();
  rei_shm *t1 = rei_spill_region_get(&h.fl, 100);
  rei_stage_retain(&h, t1);
  rei_stage_pin(&h, &token);
  rei_keeper_commit(&h, tab, 0);
  rei_shm *t2 = rei_spill_region_get(&h.fl, 100);
  rei_stage_retain(&h, t2);
  rei_keeper_commit(&h, tab, 1);
  rei_keeper_release(&h, tab, 1);      /* one in the free list */
  rei_keepers_teardown(&h, tab, 8);
  assert(drops == 1);
  rei_spill_fl_teardown(&h.fl);
  assert(h.fl.n == 0 && h.fl.total == 0 && h.fl.staging == NULL);

  puts("test_spill: ok");
  return 0;
}
