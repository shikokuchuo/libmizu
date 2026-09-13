/* stage_raw.c — the raw-tier staging reservation: the RAWVEC / arena
   RAWSPILL / region RAWSPILL / flat-SHM_VEC decision tree, single-sourced
   for the bindings (rei's stage_r.c + payload.c and pyrei's _pyrei.c staged
   this cascade by hand, kept in sync by comments). Valid only during a
   stage_fn call, like the rei_stage_* services it composes.

   The policy (both bindings' current cascade, one documented delta):
   n <= inline_max claims inline as RAWVEC (the header inline's fast path —
   this TU sees only past-inline frames). Past inline:
   - channel: the arena copy when n <= REI_ZC_FLOOR_RAW or under churn (the
     arena has no region machinery to amortize, and is churn-immune); else
     flat SHM_VEC when n >= max(inline_max, REI_ZC_FLOOR) and no churn (reap
     before the checkout, so it sees the freshest consumer-done surrenders);
     an arena retry on SHM_VEC region failure before falling back (rei's
     binding previously degraded straight to its serialized tiers there);
     else NULL.
   - pool (no arena): flat SHM_VEC on the same zc gate; else a RAWSPILL
     region (aux packs type | name_len << 8; n <= UINT32_MAX); else NULL.
   The churn read is one extern call, gated behind the zc size gate (pyrei's
   single-read discipline; rei's cascade read it twice on the churn
   fallback). n > UINT32_MAX skips the arena: hdr->len is 32-bit.

   NULL hands the object to the binding's serialized tiers — a reservation
   failure is a degradation, never an error (the serialize fallback spills
   through the same region machinery and reports its own failure). Bare
   bytes carry no identifier, so nothing is pinned; the SHM_VEC retain
   stores the producer loan (rei_reih_write zeroed the refcount word). The
   memcpy into the returned destination is the caller's. */

#include <string.h>

#include "rei_ext.h"

/* The flat REIH-layout SHM_VEC reserve: header into a spill region
   (free-list pop or fresh create), the name as the payload, aux the wire
   type | exact used bytes << 8; the data area is the returned destination.
   NULL on region failure (the caller's next tier is a copy tier). */
static uint8_t *raw_reserve_shm_vec(rei_handle *h, uint64_t n, int wire_type,
                                    rei_slot_hdr *hdr, uint8_t *payload) {
  uint64_t total = (uint64_t) REI_HEADER_SIZE + n;
  rei_shm *shm;
  if (rei_stage_spill_get(h, (size_t) total, &shm) != REI_OK) return NULL;
  uint8_t *base = (uint8_t *) shm->addr;
  rei_reih_write(base, wire_type,
                 (int64_t) (n / rei_type_elt_size(wire_type)));
  hdr->kind = REI_KIND_SHM_VEC;
  hdr->len = (uint32_t) shm->name_len;
  hdr->aux = rei_aux_shm_vec(wire_type, total);
  memcpy(payload, shm->name, shm->name_len);
  rei_stage_retain_zc(h, shm);
  return base + REI_HEADER_SIZE;
}

void *rei_stage_raw_spill(rei_handle *h, uint64_t n, int wire_type,
                          rei_slot_hdr *hdr, uint8_t *payload,
                          uint32_t inline_max) {
  const uint64_t zc_gate = (uint64_t) inline_max > REI_ZC_FLOOR ?
    (uint64_t) inline_max : (uint64_t) REI_ZC_FLOOR;
  /* zc_ok implies !churn, so the arena-first condition below needs no
     separate churn term */
  const int zc_ok = n >= zc_gate && !rei_handle_churn(h);

  if (rei_handle_kind(h) == REI_HTYPE_POOL) {
    if (zc_ok) {
      uint8_t *dst = raw_reserve_shm_vec(h, n, wire_type, hdr, payload);
      if (dst != NULL) return dst;
    }
    if (n > UINT32_MAX) return NULL;
    rei_shm *shm;
    if (rei_stage_spill_get(h, (size_t) n, &shm) != REI_OK) return NULL;
    hdr->kind = REI_KIND_RAWSPILL;
    hdr->len = (uint32_t) n;
    hdr->aux = rei_aux_rawspill_pool(wire_type, shm->name_len);
    memcpy(payload, shm->name, shm->name_len);
    rei_stage_retain(h, shm);
    return (uint8_t *) shm->addr;
  }

  uint64_t off;
  uint8_t *chunk = NULL;
  if (n <= UINT32_MAX) {
    if (!zc_ok || n <= REI_ZC_FLOOR_RAW)
      chunk = (uint8_t *) rei_stage_arena_alloc(h, REI_ALIGN64(n), &off);
    if (chunk == NULL && zc_ok) {
      rei_stage_reap(h);
      uint8_t *dst = raw_reserve_shm_vec(h, n, wire_type, hdr, payload);
      if (dst != NULL) return dst;
      chunk = (uint8_t *) rei_stage_arena_alloc(h, REI_ALIGN64(n), &off);
    }
  } else if (zc_ok) {
    rei_stage_reap(h);
    return raw_reserve_shm_vec(h, n, wire_type, hdr, payload);
  }
  if (chunk == NULL) return NULL;
  hdr->kind = REI_KIND_RAWSPILL;
  hdr->len = (uint32_t) n;
  hdr->aux = (uint64_t) (uint32_t) wire_type;
  memcpy(payload, &off, sizeof off);
  return chunk;
}
