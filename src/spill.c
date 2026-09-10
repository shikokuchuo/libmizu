/* Producer-side region lifetime: the spill free list, the lent-region
   ledger, the zc producer-loan release, the SHM_RAW consumer mapping
   cache, and the per-slot retain table. Regions are rei_shm pointers
   owned by the handle: created or popped at stage, surrendered at the
   consumer-done release points, closed + unlinked at eviction and
   teardown. A staged object that must outlive consumer-done rides the
   entry's pin — an opaque token the binding registers with
   rei_stage_pin and the core hands to the binding's drop hook at every
   release point (a binding pins the staged object, whose serialized
   stream may carry hook-emitted identifiers). */

#include <stdlib.h>
#if defined(__linux__)
#include <sys/mman.h>
#endif
#include "internal.h"

// Region teardown ------------------------------------------------------------

/* Producer-side destroy: release the name (POSIX: unlink + the macOS
   registry balance) and the mapping. Windows folds both into
   rei_shm_close (the creator handle rides the struct; closing it is the
   unlink). */
static void rei_region_destroy(rei_shm *shm) {
  if (shm == NULL) return;
#ifndef _WIN32
  rei_shm_host_release(shm);
#endif
  rei_shm_close_stack(shm, 0);
  free(shm);
}

void rei_region_unlink(rei_shm *shm) {
  rei_shm_host_release(shm);
#ifdef _WIN32
  shm->handle = NULL;   /* released; rei_shm_close must not re-close it */
#endif
}

// Handle queries (rei_ext.h) ---------------------------------------------------

/* Field reads over the private handle struct, so bindings never open it.
   rei_handle_churn is deliberately a plain extern call: bindings gate it
   behind their payload size check, so it runs only for payloads already
   proven large (where the layout write dominates the call). */
int rei_handle_kind(const rei_handle *h) {
  return h->htype;
}

int rei_handle_churn(const rei_handle *h) {
  return h->fl.churn;
}

void *rei_handle_binding_ctx(const rei_handle *h) {
  return h->binding.ctx;
}

void rei_handle_spill_info(const rei_handle *h, uint32_t *fl_entries,
                           uint32_t *ledger_entries) {
  *fl_entries = h->fl.n;
  *ledger_entries = h->fl.led_n;
}

// Free list --------------------------------------------------------------------

static size_t spill_round(size_t n) {
  size_t c = REI_SPILL_FL_FLOOR;
  while (c < n) c <<= 1;
  return c;
}

/* The free-list insert under the size-class and total-byte caps (evicting
   largest-oldest), shared by keeper releases and the ledger sweep. A
   region that doesn't fit is closed + unlinked in place. */
void rei_spill_fl_insert(rei_spill_fl *fl, rei_shm *shm) {
  size_t size = shm->size;
  if (size > REI_SPILL_FL_BYTES) {
    rei_region_destroy(shm);
    return;
  }
  int cls = 0, slot = -1;
  for (int i = 0; i < REI_SPILL_FL_MAX; i++) {
    if (fl->size[i] == 0) slot = i;
    else cls += spill_round(fl->size[i]) == spill_round(size);
  }
  if (cls >= REI_SPILL_FL_CLASS) {
    rei_region_destroy(shm);
    return;
  }
  /* total-byte cap: evict largest (oldest among equals) until it fits */
  while (fl->n > 0 && fl->total + size > REI_SPILL_FL_BYTES) {
    int vic = -1;
    for (int i = 0; i < REI_SPILL_FL_MAX; i++) {
      if (fl->size[i] == 0) continue;
      if (vic < 0 || fl->size[i] > fl->size[vic] ||
          (fl->size[i] == fl->size[vic] && fl->stamp[i] < fl->stamp[vic]))
        vic = i;
    }
    rei_region_destroy(fl->regions[vic]);
    fl->regions[vic] = NULL;
    fl->total -= fl->size[vic];
    fl->size[vic] = 0;
    fl->n--;
    slot = vic;
  }
  if (slot < 0) {                        /* every entry occupied */
    rei_region_destroy(shm);
    return;
  }
  fl->regions[slot] = shm;
  fl->size[slot] = size;
  fl->stamp[slot] = ++fl->tick;
  fl->total += size;
  fl->n++;
#if defined(__linux__) && defined(MADV_COLLAPSE)
  /* Collapse a proved-reusable region to huge pages (Linux >= 6.1): on
     stock Linux shmem THP is off, so the create-time MADV_HUGEPAGE is
     inert and a reused region would stay on 4 KiB pages forever. At
     insert — not create — so only regions that completed a consumer-done
     cycle pay; under zc churn lent regions never reach here. Idempotent
     and benign on failure. */
  if (size >= ((size_t) 2 << 20))
    (void) madvise(shm->addr, size, MADV_COLLAPSE);
#endif
}

/* Smallest entry with size >= n, removed from the list. A miss runs a
   full ledger sweep (zero-count lent regions rejoin here) and retries
   once — the free-list-miss full sweep of the release protocol. */
static rei_shm *spill_fl_pop(rei_spill_fl *fl, size_t n) {
  for (int attempt = 0; attempt < 2; attempt++) {
    int best = -1;
    for (int i = 0; i < REI_SPILL_FL_MAX; i++) {
      if (fl->size[i] < n || fl->size[i] == 0) continue;
      if (best < 0 || fl->size[i] < fl->size[best]) best = i;
    }
    if (best >= 0) {
      rei_shm *shm = fl->regions[best];
      fl->regions[best] = NULL;
      fl->total -= fl->size[best];
      fl->size[best] = 0;
      fl->n--;
      return shm;
    }
    if (attempt > 0 || fl->led_n == 0) break;
    rei_ledger_sweep(fl, REI_LEDGER_MAX);
  }
#ifdef __linux__
  /* A miss with lent regions still outstanding: the sweep just proved
     consumer-side views outlive their traffic. The signal is Linux-only
     because only there is a fresh region dear: the create pre-faults
     every page (posix_fallocate + MAP_POPULATE, SIGBUS-proofing tmpfs),
     so a fresh region per SHM_VEC payload pays a full extra pass over
     the bytes and the copy tiers' deterministic reuse wins. macOS and
     Windows creates are lazy — the layout write faults the pages it
     touches anyway — so SHM_VEC (one layout write, no receive copy)
     beats the fallback even under churn. */
  if (fl->led_n > 0) fl->churn = 1;
#endif
  return NULL;
}

/* Pop-or-create a spill region: the pow2 size class when a free list is
   in play so nearby payload sizes hit it later, exact bytes for one-shot
   regions. The checkout is recorded in fl->staging — committed to the
   retain table (rei_keeper_commit), discarded (rei_keeper_discard), or
   abandoned to rei_stage_rollback. NULL + the thread-local error slot on
   create failure. */
rei_shm *rei_spill_region_get(rei_spill_fl *fl, size_t n) {
  rei_shm *shm = NULL;
  if (fl != NULL) {
    fl->last_reused = 0;
    shm = spill_fl_pop(fl, n);
  }
  if (shm != NULL) {
    fl->last_reused = 1;
    fl->hits++;
  } else {
    size_t cap = fl != NULL && n <= REI_SPILL_FL_BYTES ? spill_round(n) : n;
    int rc = rei_shm_create_heap(&shm, cap);
    if (rc != REI_ERRCAT_NONE) {
      const char *summary, *hint;
      rei_err_describe((rei_errcat) rc, &summary, &hint);
      rei_err_record_tls((rei_errcat) rc,
                         "cannot create payload region (%llu bytes): %s%s%s",
                         (unsigned long long) cap, summary,
                         hint[0] != '\0' ? ". " : "", hint);
      return NULL;
    }
  }
  if (fl != NULL) fl->staging = shm;
  return shm;
}

// Staging services (rei.h) -----------------------------------------------------

rei_status rei_stage_spill_get(rei_handle *h, size_t n, rei_shm **out) {
  rei_shm *shm = rei_spill_region_get(&h->fl, n);
  if (shm == NULL) {
    rei_err_record(h, rei_last_error_category(), "%s",
                   rei_last_error_message());
    return REI_ERR;
  }
  *out = shm;
  return REI_OK;
}

/* The retain commits the checkout's kind; `region` must be the current
   checkout. A mismatch is a binding bug: ignored, so the region stays
   uncommitted and rolls back at the next verb instead of corrupting a
   table slot. */
void rei_stage_retain(rei_handle *h, rei_shm *region) {
  if (region != h->fl.staging) return;
  h->fl.staging_kind = REI_KEEP_SPILL;
}

void rei_stage_retain_zc(rei_handle *h, rei_shm *region) {
  if (region != h->fl.staging) return;
  /* The producer loan: the refcount field's only initialization and its
     own reference — a recycled region carries a stale count and flags. */
  atomic_store_explicit(rei_zc_rc(region->addr), 1, memory_order_relaxed);
  atomic_store_explicit(rei_zc_flags_(region->addr), 0, memory_order_relaxed);
  h->fl.staging_kind = REI_KEEP_ZC;
}

void rei_stage_pin(rei_handle *h, void *pin) {
  h->fl.staging_pin = pin;
  if (h->fl.staging_kind == REI_KEEP_FREE)
    h->fl.staging_kind = REI_KEEP_PIN;
}

// Retain table -----------------------------------------------------------------

/* Roll back an abandoned checkout (a stage that raised or failed before
   commit): the region rejoins the free list intact and a registered pin
   drops through the binding's hook. Runs at the top of every staging
   verb and at handle teardown, so a handle that stages-then-abandons
   never leaks. */
void rei_stage_rollback(rei_handle *h) {
  rei_spill_fl *fl = &h->fl;
  rei_shm *shm = fl->staging;
  void *pin = fl->staging_pin;
  fl->staging = NULL;
  fl->staging_pin = NULL;
  fl->staging_kind = REI_KEEP_FREE;
  if (pin != NULL && h->binding.drop != NULL)
    h->binding.drop(h->binding.ctx, pin);
  if (shm != NULL) rei_spill_fl_insert(fl, shm);
}

/* Commit the current staging checkout into the (FREE) slot. Called by
   the transport after stage_fn returns 0 and the frame publishes. */
void rei_keeper_commit(rei_handle *h, rei_keeper *tab, uint32_t at) {
  rei_spill_fl *fl = &h->fl;
  tab[at].region = fl->staging;
  tab[at].pin = fl->staging_pin;
  tab[at].key = -1;
  tab[at].kind = fl->staging_kind;
  fl->staging = NULL;
  fl->staging_pin = NULL;
  fl->staging_kind = REI_KEEP_FREE;
}

/* The consumer-done release: drop the pin through the binding's hook,
   surrender the region per its kind (SPILL to the free list, ZC through
   the refcount protocol), mark the slot FREE. */
void rei_keeper_release(rei_handle *h, rei_keeper *tab, uint32_t at) {
  rei_keeper *k = &tab[at];
  uint8_t kind = k->kind;
  if (kind == REI_KEEP_FREE) return;
  rei_shm *shm = k->region;
  int32_t key = k->key;
  void *pin = k->pin;
  k->kind = REI_KEEP_FREE;
  k->region = NULL;
  k->key = -1;
  k->pin = NULL;
  if (pin != NULL && h->binding.drop != NULL)
    h->binding.drop(h->binding.ctx, pin);
  if (shm == NULL) return;
  if (kind == REI_KEEP_ZC) rei_zc_release(&h->fl, shm, key);
  else rei_spill_fl_insert(&h->fl, shm);
}

/* The never-published discard (a cancelled pool publish): surrender the
   staged region per its kind, drop the pin, clear the checkout. */
void rei_keeper_discard(rei_handle *h) {
  rei_spill_fl *fl = &h->fl;
  uint8_t kind = fl->staging_kind;
  rei_shm *shm = fl->staging;
  void *pin = fl->staging_pin;
  fl->staging = NULL;
  fl->staging_pin = NULL;
  fl->staging_kind = REI_KEEP_FREE;
  if (pin != NULL && h->binding.drop != NULL)
    h->binding.drop(h->binding.ctx, pin);
  if (shm == NULL) return;
  if (kind == REI_KEEP_ZC) rei_zc_release(fl, shm, -1);
  else rei_spill_fl_insert(fl, shm);
}

/* Handle teardown: close + unlink every retained region, drop every pin.
   No free-list surrender and no refcount sub — the handle is dying. */
void rei_keepers_teardown(rei_handle *h, rei_keeper *tab, uint32_t n) {
  if (tab == NULL) return;
  for (uint32_t i = 0; i < n; i++) {
    if (tab[i].kind == REI_KEEP_FREE) continue;
    rei_region_destroy(tab[i].region);
    if (tab[i].pin != NULL && h->binding.drop != NULL)
      h->binding.drop(h->binding.ctx, tab[i].pin);
    tab[i].kind = REI_KEEP_FREE;
    tab[i].region = NULL;
    tab[i].key = -1;
    tab[i].pin = NULL;
  }
}

void rei_spill_fl_teardown(rei_spill_fl *fl) {
  rei_region_destroy(fl->staging);
  fl->staging = NULL;
  for (int i = 0; i < REI_SPILL_FL_MAX; i++) {
    rei_region_destroy(fl->regions[i]);
    fl->regions[i] = NULL;
    fl->size[i] = 0;
  }
  fl->n = 0;
  fl->total = 0;
  for (int i = 0; i < REI_LEDGER_MAX; i++) {
    rei_region_destroy(fl->led_regions[i]);
    fl->led_regions[i] = NULL;
  }
  fl->led_n = 0;
  for (uint32_t i = 0; i < fl->dropped_n; i++) {
    rei_region_destroy(fl->dropped[i]);
  }
  free(fl->dropped);
  fl->dropped = NULL;
  fl->dropped_n = 0;
  fl->dropped_cap = 0;
}

// Consumer mapping cache ---------------------------------------------------------

/* The name-keyed lookup: the mapping on a hit (stamp bumped), NULL on a
   miss (the caller re-opens and re-stores). Owned mappings never
   finalize, so a hit is always live. */
rei_shm *rei_oc_lookup(rei_open_cache *oc, const unsigned char *name,
                       uint32_t len) {
  for (int i = 0; i < REI_OPEN_CACHE_MAX; i++)
    if (oc->name_len[i] == len &&
        memcmp(oc->names[i], name, len) == 0) {
      oc->stamp[i] = ++oc->tick;
      oc->hits++;
      return oc->maps[i];
    }
  return NULL;
}

/* The LRU store: takes ownership of shm; an evicted entry is closed. */
void rei_oc_store(rei_open_cache *oc, rei_shm *shm) {
  int slot = 0;
  for (int i = 0; i < REI_OPEN_CACHE_MAX; i++) {
    if (oc->name_len[i] == 0) {
      slot = i;
      break;
    }
    if (oc->stamp[i] < oc->stamp[slot]) slot = i;
  }
  if (oc->name_len[slot] != 0)           /* evicted LRU */
    rei_shm_close(oc->maps[slot], 0);
  oc->maps[slot] = shm;
  memcpy(oc->names[slot], shm->name, shm->name_len);
  oc->name_len[slot] = shm->name_len;
  oc->stamp[slot] = ++oc->tick;
  oc->misses++;
}

void rei_oc_teardown(rei_open_cache *oc) {
  for (int i = 0; i < REI_OPEN_CACHE_MAX; i++) {
    if (oc->maps[i] != NULL) {           /* consumer mappings: no unlink */
      rei_shm_close(oc->maps[i], 0);
      oc->maps[i] = NULL;
    }
    oc->name_len[i] = 0;
  }
}

/* The read-side service (rei.h): a borrowed consumer mapping for a
   SHM_RAW-class payload name, from the handle's open cache. */
rei_shm *rei_read_region(rei_read_ctx *ctx, const uint8_t *name,
                         uint32_t len) {
  rei_handle *h = ctx->handle;
  rei_shm *shm = rei_oc_lookup(&h->oc, name, len);
  if (shm == NULL) {
    if (len == 0 || len >= REI_NAME_MAX) {
      ctx->gone = 1;
      return NULL;
    }
    char nbuf[REI_NAME_MAX];
    memcpy(nbuf, name, len);
    nbuf[len] = '\0';
    shm = rei_shm_open_ro_heap(nbuf);
    if (shm == NULL) {
      /* the region died with its creator (Win32 mappings cannot outlive
         theirs): report through ctx->gone; read_fn propagates */
      ctx->gone = 1;
      return NULL;
    }
    rei_oc_store(&h->oc, shm);
  }
  return shm;
}

// ZC producer-loan release and the lent-region ledger -----------------------------

/* A ledger-overflow drop: the region's lent views may still be re-sent by
   reference (a REF in flight resolves by name), so the name must outlive
   any possible resolve. The mapping closes here; the unlink is deferred
   to handle teardown — the same point a tracked ledger region's name
   dies. Live views keep their own mappings; only recycling is forfeited.
   An alloc failure forfeits the REF window, never the region. */
static void rei_overflow_drop(rei_spill_fl *fl, rei_shm *shm) {
  if (fl->dropped_n == fl->dropped_cap) {
    uint32_t cap = fl->dropped_cap == 0 ? 8 : 2 * fl->dropped_cap;
    rei_shm **next = realloc(fl->dropped, cap * sizeof(*next));
    if (next == NULL) {
      rei_region_destroy(shm);
      return;
    }
    fl->dropped = next;
    fl->dropped_cap = cap;
  }
  rei_shm_close_stack(shm, 0);   /* keep name + pid; drop only the mapping */
  fl->dropped[fl->dropped_n++] = shm;
}

/* The producer-loan drop at a consumer-done release point: refcount sub,
   then the free list on 0 (no live views) or the lent-region ledger
   otherwise. A full ledger drops the region to rei_overflow_drop. */
void rei_zc_release(rei_spill_fl *fl, rei_shm *shm, int32_t key) {
  uint32_t prev =
    atomic_fetch_sub_explicit(rei_zc_rc(shm->addr), 1, memory_order_acq_rel);
  if (prev <= 1) {
    rei_spill_fl_insert(fl, shm);
    return;
  }
  if (fl->led_n >= REI_LEDGER_MAX) {
    rei_overflow_drop(fl, shm);
    return;
  }
  fl->led_regions[fl->led_n] = shm;
  fl->led_key[fl->led_n] = key;
  fl->led_n++;
}

static void rei_ledger_drop(rei_spill_fl *fl, uint32_t i) {
  fl->led_n--;
  fl->led_regions[i] = fl->led_regions[fl->led_n];
  fl->led_regions[fl->led_n] = NULL;
  fl->led_key[i] = fl->led_key[fl->led_n];
}

void rei_ledger_sweep(rei_spill_fl *fl, uint32_t quota) {
  uint32_t i = 0, visited = 0;
  while (i < fl->led_n && visited < quota) {
    rei_shm *shm = fl->led_regions[i];
    visited++;
    uint32_t count =
      atomic_load_explicit(rei_zc_rc(shm->addr), memory_order_acquire);
    if (count == 0) {
      rei_spill_fl_insert(fl, shm);
      rei_ledger_drop(fl, i);
      fl->churn = 0;   /* releases are landing: zero-copy reuse is viable */
    } else {
      i++;
    }
  }
}

/* The death backstop, after the liveness verdict: a dead consumer's
   finalizers never ran, so its counts leaked. Unflagged entries rejoin
   the free list (the stage-time store of 1 re-initializes) — sound only
   while the dead peer is the sole possible view-holder. REFHELD entries
   have a wider holder set: leak the count and kill the name (live views
   keep their own mappings), never force-reclaim. */
void rei_ledger_force(rei_spill_fl *fl, int32_t key) {
  uint32_t i = 0;
  while (i < fl->led_n) {
    if (key >= 0 && fl->led_key[i] != key) {
      i++;
      continue;
    }
    rei_shm *shm = fl->led_regions[i];
    uint32_t flags =
      atomic_load_explicit(rei_zc_flags_(shm->addr), memory_order_acquire);
    if (flags & REI_ZC_FLAG_REFHELD) {
      rei_region_destroy(shm);
    } else {
      rei_spill_fl_insert(fl, shm);
      fl->churn = 0;
    }
    rei_ledger_drop(fl, i);
  }
}
