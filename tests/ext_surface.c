/* The ext-tier contract proof: a TU including only mizu.h + mizu_ext.h
   (the Makefile rule puts -Iinclude alone on the path, so internal.h is
   unreachable) that references every mizu_ext.h symbol, then runs
   functional checks on the dual-form accessors.

   Two modes:
   - default (also how CI compiles it against the amalgamation): the
     dual-form accessors resolve to the header's static inlines, as a C
     consumer's do.
   - EXT_PROBE_EXPORTS (make test-ext): MIZU_EXT_NO_INLINES hides the
     inlines, so the address-of references below force the exported
     symbols (src/ext.c) to link — the proof that the FFI forms exist
     in the static/shared library.

   Both modes run the same assertions, so the inline and exported forms
   are checked for agreement. */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef EXT_PROBE_EXPORTS
#  define MIZU_EXT_NO_INLINES
#endif
#include "mizu.h"
#include "mizu_ext.h"

/* C11 6.3.2.3/8: function pointers convert between types and back; only
   calling through an incompatible type is UB (never done here). */
typedef void (*mizu_any_fn)(void);

static int sink(mizu_any_fn f) { return f != NULL; }

int main(void) {
  int refs = 0;

  /* The binding seam and the bytes binding. */
  refs += sink((mizu_any_fn) &mizu_binding_init);
  refs += sink((mizu_any_fn) &mizu_stage_arena_alloc);
  refs += sink((mizu_any_fn) &mizu_stage_spill_get);
  refs += sink((mizu_any_fn) &mizu_stage_retain);
  refs += sink((mizu_any_fn) &mizu_stage_retain_zc);
  refs += sink((mizu_any_fn) &mizu_stage_pin);
  refs += sink((mizu_any_fn) &mizu_stage_reap);
  refs += sink((mizu_any_fn) &mizu_read_region);
  refs += sink((mizu_any_fn) &mizu_result_publish);
  refs += sink((mizu_any_fn) &mizu_result_publish_err);
  refs += sink((mizu_any_fn) &mizu_result_publish_died);
  refs += sink((mizu_any_fn) &mizu_binding_bytes);
  refs += sink((mizu_any_fn) &mizu_bytes_free);

  /* Handle queries. */
  refs += sink((mizu_any_fn) &mizu_handle_kind);
  refs += sink((mizu_any_fn) &mizu_handle_churn);
  refs += sink((mizu_any_fn) &mizu_handle_keep_out);
  refs += sink((mizu_any_fn) &mizu_handle_spill_info);

  /* Parker, death watch, liveness, preamble. */
  refs += sink((mizu_any_fn) &mizu_parker_attach);
  refs += sink((mizu_any_fn) &mizu_parker_detach);
  refs += sink((mizu_any_fn) &mizu_park);
  refs += sink((mizu_any_fn) &mizu_unpark);
  refs += sink((mizu_any_fn) &mizu_death_watch_start);
  refs += sink((mizu_any_fn) &mizu_death_watch_stop);
  refs += sink((mizu_any_fn) &mizu_death_listener_teardown);
  refs += sink((mizu_any_fn) &mizu_live_dir);
  refs += sink((mizu_any_fn) &mizu_live_open);
  refs += sink((mizu_any_fn) &mizu_live_try);
  refs += sink((mizu_any_fn) &mizu_live_close);
  refs += sink((mizu_any_fn) &mizu_preamble_write);
  refs += sink((mizu_any_fn) &mizu_preamble_validate);

  /* Pool map support and the unwind path. */
  refs += sink((mizu_any_fn) &mizu_pool_signals);
  refs += sink((mizu_any_fn) &mizu_pool_help_once);
  refs += sink((mizu_any_fn) &mizu_pool_deque_pull);
  refs += sink((mizu_any_fn) &mizu_pool_map_caps);
  refs += sink((mizu_any_fn) &mizu_pool_submit_flags);
  refs += sink((mizu_any_fn) &mizu_pool_eval_mark);
  refs += sink((mizu_any_fn) &mizu_pool_unwind_sink);

  /* Utilities. */
  refs += sink((mizu_any_fn) &mizu_now);
  refs += sink((mizu_any_fn) &mizu_self_pid);
  refs += sink((mizu_any_fn) &mizu_err_describe);
  refs += sink((mizu_any_fn) &mizu_rng_jump);
  refs += sink((mizu_any_fn) &mizu_tune);

  /* The dual-form accessors and wire helpers: the header's static
     inlines by default, the exported symbols under EXT_PROBE_EXPORTS. */
  refs += sink((mizu_any_fn) &mizu_parker_snapshot);
  refs += sink((mizu_any_fn) &mizu_zc_rc);
  refs += sink((mizu_any_fn) &mizu_zc_flags_);
  refs += sink((mizu_any_fn) &mizu_timeout_ms);
  refs += sink((mizu_any_fn) &mizu_store_na_real);
  refs += sink((mizu_any_fn) &mizu_aux_rawspill_pool);
  refs += sink((mizu_any_fn) &mizu_aux_shm_vec);
  refs += sink((mizu_any_fn) &mizu_aux_type);
  refs += sink((mizu_any_fn) &mizu_aux_hi);
  refs += sink((mizu_any_fn) &mizu_mizh_write);
  refs += sink((mizu_any_fn) &mizu_mizh_check);
  refs += sink((mizu_any_fn) &mizu_mizh_validity_set);
  refs += sink((mizu_any_fn) &mizu_mizs_geometry);
  refs += sink((mizu_any_fn) &mizu_mizs_check);
  refs += sink((mizu_any_fn) &mizu_mizl_check);
  refs += sink((mizu_any_fn) &mizu_mizl_elem);
  refs += sink((mizu_any_fn) &mizu_na_build);
  refs += sink((mizu_any_fn) &mizu_na_apply);

  /* The identity words. */
  refs += sink((mizu_any_fn) &mizu_channel_peer_ident);
  refs += sink((mizu_any_fn) &mizu_pool_worker_ident);

  /* The interchange cursor and emit helpers (the put helpers are
     dual-form: the header's static inlines by default, the exported
     symbols under EXT_PROBE_EXPORTS). */
  refs += sink((mizu_any_fn) &mizu_ix_open);
  refs += sink((mizu_any_fn) &mizu_ix_next);
  refs += sink((mizu_any_fn) &mizu_ix_end);
  refs += sink((mizu_any_fn) &mizu_ix_put_header);
  refs += sink((mizu_any_fn) &mizu_ix_put_nil);
  refs += sink((mizu_any_fn) &mizu_ix_put_lgl);
  refs += sink((mizu_any_fn) &mizu_ix_put_int);
  refs += sink((mizu_any_fn) &mizu_ix_put_real);
  refs += sink((mizu_any_fn) &mizu_ix_put_cplx);
  refs += sink((mizu_any_fn) &mizu_ix_put_str);
  refs += sink((mizu_any_fn) &mizu_ix_put_bytes);
  refs += sink((mizu_any_fn) &mizu_ix_put_vec);
  refs += sink((mizu_any_fn) &mizu_ix_put_strv_begin);
  refs += sink((mizu_any_fn) &mizu_ix_put_strelt);
  refs += sink((mizu_any_fn) &mizu_ix_put_list_begin);
  refs += sink((mizu_any_fn) &mizu_ix_put_dict_begin);
  refs += sink((mizu_any_fn) &mizu_ix_put_key);
  refs += sink((mizu_any_fn) &mizu_ix_put_attr);
  refs += sink((mizu_any_fn) &mizu_ix_put_err);
  refs += sink((mizu_any_fn) &mizu_ix_put_task);

  /* The raw-tier staging reservation (the header inline by default, the
     exported form under EXT_PROBE_EXPORTS; the slow path is extern-only). */
  refs += sink((mizu_any_fn) &mizu_stage_raw);
  refs += sink((mizu_any_fn) &mizu_stage_raw_spill);

  /* The map morsel protocol. */
  refs += sink((mizu_any_fn) &mizu_morsel_layout);
  refs += sink((mizu_any_fn) &mizu_morsel_hdr_check);
  refs += sink((mizu_any_fn) &mizu_morsel_sizer_init);
  refs += sink((mizu_any_fn) &mizu_morsel_next);
  refs += sink((mizu_any_fn) &mizu_morsel_reset);
  refs += sink((mizu_any_fn) &mizu_morsel_abandon);
  refs += sink((mizu_any_fn) &mizu_morsel_cancel_set);
  refs += sink((mizu_any_fn) &mizu_morsel_cancel_get);
  refs += sink((mizu_any_fn) &mizu_morsel_generation);
  refs += sink((mizu_any_fn) &mizu_morsel_cursor);
  refs += sink((mizu_any_fn) &mizu_morsel_claim);
  refs += sink((mizu_any_fn) &mizu_morsel_span_of);
  refs += sink((mizu_any_fn) &mizu_morsel_lost);

  /* A taste of the stable tier: the proof links both headers' surface. */
  refs += sink((mizu_any_fn) &mizu_shm_open_view_flags);

  assert(refs == 98);

  /* Every ext-tier type is complete here (internal.h is absent). */
  size_t sizes = sizeof(mizu_binding) + sizeof(mizu_read_ctx) +
    sizeof(mizu_result_sink) + sizeof(mizu_shm) + sizeof(mizu_parker) +
    sizeof(mizu_pool_sig) + sizeof(mizu_bytes) + sizeof(mizu_morsel_hdr) +
    sizeof(mizu_morsel_sizer) + sizeof(mizu_morsel_span) +
    sizeof(mizu_mizs_geom) + sizeof(mizu_mizl_entry) + sizeof(mizu_ix) +
    sizeof(mizu_ix_item);
  assert(sizes > 0);

  /* Macro surface. */
  assert(sizeof(MIZU_PREFIX_LITERAL) > 1);
  assert(MIZU_ALIGN64(1) == 64 && MIZU_ALIGN64(64) == 64 &&
         MIZU_ALIGN64(65) == 128);
  assert(MIZU_ZC_FLOOR < MIZU_ZC_FLOOR_RAW);
  assert(MIZU_OPEN_CACHE_MAX == 16);
  assert(MIZU_CODEC_MAGIC == 'R');
  assert(MIZU_PYMIZU_CODEC_MAGIC == 'P');
  assert(MIZU_INTEROP_MAGIC == 'I');
  assert(MIZU_MORSEL_MAGIC == 0x4D495A4Du);
  assert(MIZU_HTYPE_CHANNEL != MIZU_HTYPE_POOL);
  assert(MIZU_PARK_WOKEN != MIZU_PARK_TIMEOUT);
  assert(MIZU_LIVE_ACQUIRED != MIZU_LIVE_HELD);

  /* The registries and the identity word. */
  assert(MIZU_LANG_NONE == 0 && MIZU_LANG_BYTES == 1 && MIZU_LANG_R == 2 &&
         MIZU_LANG_PYTHON == 3);
  assert(MIZU_CAP_MIZS == 1u && MIZU_CAP_ATTRS == 2u && MIZU_CAP_MIZL == 4u);
  assert(MIZU_IDENT(MIZU_LANG_R, MIZU_CAP_MIZS | MIZU_CAP_MIZL) ==
         ((uint64_t) 2 | ((uint64_t) 5 << 32)));
  assert((uint8_t) MIZU_IDENT(MIZU_LANG_PYTHON, 0) == MIZU_LANG_PYTHON);
  assert(MIZU_CE_NATIVE == 0 && MIZU_CE_UTF8 == 1 && MIZU_CE_LATIN1 == 2 &&
         MIZU_CE_BYTES == 3);

  /* mizu_binding_init zeroes and size-stamps. */
  mizu_binding b;
  memset(&b, 0xff, sizeof b);
  mizu_binding_init(&b);
  assert(b.size == sizeof b && b.stage == NULL && b.read == NULL &&
         b.exec == NULL && b.ctx == NULL && b.ident == 0);

  /* The dual forms compute the wire-format offsets, in either mode. */
  unsigned char region[MIZU_HEADER_SIZE];
  memset(region, 0, sizeof region);
  atomic_store_explicit(mizu_zc_rc(region), 7, memory_order_relaxed);
  atomic_store_explicit(mizu_zc_flags_(region), 3, memory_order_relaxed);
  uint32_t rc_word;
  memcpy(&rc_word, region + MIZU_ZC_REFCOUNT_OFF, sizeof rc_word);
  assert(rc_word == 7);
  uint32_t fl_word;
  memcpy(&fl_word, region + MIZU_ZC_FLAGS_OFF, sizeof fl_word);
  assert(fl_word == 3);

  uint32_t epoch = 42;
  mizu_parker pk;
  memset(&pk, 0, sizeof pk);
  pk.epoch = (_Atomic uint32_t *) &epoch;
  assert(mizu_parker_snapshot(&pk) == 42);

  /* The wire helpers, in either mode (the edge cases live in
     tests/unit/test_wire.c; here a smoke pass proves the two forms). */
  assert(mizu_timeout_ms(1.5) == 1500.0);
  assert(mizu_timeout_ms(NAN) == -1 && mizu_timeout_ms(-1) == 0);
  unsigned char na8[8];
  mizu_store_na_real(na8);
  uint64_t na_bits;
  memcpy(&na_bits, na8, 8);
  assert(na_bits == MIZU_NA_REAL_BITS);
  assert(mizu_aux_rawspill_pool(MIZU_TYPE_INT64, 27) ==
         ((uint64_t) 32 | ((uint64_t) 27 << 8)));
  assert(mizu_aux_shm_vec(MIZU_TYPE_REAL, 1u << 20) ==
         ((uint64_t) 14 | ((uint64_t) (1u << 20) << 8)));
  assert(mizu_aux_type(mizu_aux_shm_vec(MIZU_TYPE_REAL, 1u << 20)) == 14);
  assert(mizu_aux_hi(mizu_aux_rawspill_pool(MIZU_TYPE_INT64, 27)) == 27);
  unsigned char mizh[MIZU_HEADER_SIZE + 24];
  memset(mizh, 0xAA, sizeof mizh);
  mizu_mizh_write(mizh, MIZU_TYPE_REAL, 3);
  int wtype = 0;
  int64_t nelem = 0;
  int64_t valid[2] = { 1, 1 };
  assert(mizu_mizh_check(mizh, sizeof mizh, &wtype, &nelem, valid) == 0);
  assert(wtype == MIZU_TYPE_REAL && nelem == 3);
  assert(valid[0] == 0 && valid[1] == 0);   /* absent */
  mizu_mizh_validity_set(mizh, 0, -1);      /* known-NA-free */
  assert(mizu_mizh_check(mizh, sizeof mizh, &wtype, &nelem, valid) == 0 &&
         valid[0] == 0 && valid[1] == -1);
  uint32_t rc_after_write;
  memcpy(&rc_after_write, mizh + MIZU_ZC_REFCOUNT_OFF, sizeof rc_after_write);
  assert(rc_after_write == 0);   /* the reserved band write zeroed it */

  /* The interchange emit helpers, in either mode: the counting form and
     the writing form agree, and the cursor reads back what was written. */
  unsigned char ixb[64];
  size_t off = mizu_ix_put_header(ixb);
  off += mizu_ix_put_list_begin(ixb + off, 3);
  off += mizu_ix_put_int(ixb + off, -7000000000LL);
  off += mizu_ix_put_str(ixb + off, "hi", 2);
  off += mizu_ix_put_lgl(ixb + off, 2);
  assert(mizu_ix_put_header(NULL) + mizu_ix_put_list_begin(NULL, 3) +
         mizu_ix_put_int(NULL, -7000000000LL) +
         mizu_ix_put_str(NULL, "hi", 2) + mizu_ix_put_lgl(NULL, 2) == off);
  mizu_ix cur;
  assert(mizu_ix_open(&cur, ixb, off) == MIZU_OK);
  mizu_ix_item it;
  assert(mizu_ix_next(&cur, &it) == MIZU_OK && it.kind == MIZU_IX_LIST &&
         it.count == 3);
  assert(mizu_ix_next(&cur, &it) == MIZU_OK && it.kind == MIZU_IX_INT);
  int64_t back;
  memcpy(&back, &it.u64[0], 8);
  assert(back == -7000000000LL);
  assert(mizu_ix_next(&cur, &it) == MIZU_OK && it.kind == MIZU_IX_STR1 &&
         it.len == 2 && memcmp(it.ptr, "hi", 2) == 0);
  assert(mizu_ix_next(&cur, &it) == MIZU_OK && it.kind == MIZU_IX_LGL &&
         it.u64[0] == 2);
  assert(mizu_ix_end(&cur) == MIZU_OK);

  /* A taste of the stable tier: the proof links both headers' surface. */
  assert(mizu_version() != NULL);
  assert(mizu_now() > 0.0);
  assert(mizu_self_pid() > 0);
  const char *summary, *hint;
  mizu_err_describe(MIZU_ERRCAT_NOSPACE, &summary, &hint);
  assert(summary != NULL && hint != NULL);

  printf("ext_surface: OK (%d symbol refs, %s mode)\n", refs,
#ifdef EXT_PROBE_EXPORTS
         "exports-probe"
#else
         "inline"
#endif
  );
  return 0;
}
