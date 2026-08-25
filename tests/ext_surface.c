/* The ext-tier contract proof: a TU including only rei.h + rei_ext.h
   (the Makefile rule puts -Iinclude alone on the path, so internal.h is
   unreachable) that references every rei_ext.h symbol, then runs
   functional checks on the dual-form accessors.

   Two modes:
   - default (also how CI compiles it against the amalgamation): the
     dual-form accessors resolve to the header's static inlines, as a C
     consumer's do.
   - EXT_PROBE_EXPORTS (make test-ext): REI_EXT_NO_INLINES hides the
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
#  define REI_EXT_NO_INLINES
#endif
#include "rei.h"
#include "rei_ext.h"

/* C11 6.3.2.3/8: function pointers convert between types and back; only
   calling through an incompatible type is UB (never done here). */
typedef void (*rei_any_fn)(void);

static int sink(rei_any_fn f) { return f != NULL; }

int main(void) {
  int refs = 0;

  /* The binding seam and the bytes binding. */
  refs += sink((rei_any_fn) &rei_binding_init);
  refs += sink((rei_any_fn) &rei_stage_arena_alloc);
  refs += sink((rei_any_fn) &rei_stage_spill_get);
  refs += sink((rei_any_fn) &rei_stage_retain);
  refs += sink((rei_any_fn) &rei_stage_retain_zc);
  refs += sink((rei_any_fn) &rei_stage_pin);
  refs += sink((rei_any_fn) &rei_stage_reap);
  refs += sink((rei_any_fn) &rei_read_region);
  refs += sink((rei_any_fn) &rei_result_publish);
  refs += sink((rei_any_fn) &rei_result_publish_err);
  refs += sink((rei_any_fn) &rei_result_publish_died);
  refs += sink((rei_any_fn) &rei_binding_bytes);
  refs += sink((rei_any_fn) &rei_bytes_free);

  /* Handle queries. */
  refs += sink((rei_any_fn) &rei_handle_kind);
  refs += sink((rei_any_fn) &rei_handle_churn);
  refs += sink((rei_any_fn) &rei_handle_spill_info);

  /* Parker, death watch, liveness, preamble. */
  refs += sink((rei_any_fn) &rei_parker_attach);
  refs += sink((rei_any_fn) &rei_parker_detach);
  refs += sink((rei_any_fn) &rei_park);
  refs += sink((rei_any_fn) &rei_unpark);
  refs += sink((rei_any_fn) &rei_death_watch_start);
  refs += sink((rei_any_fn) &rei_death_watch_stop);
  refs += sink((rei_any_fn) &rei_death_listener_teardown);
  refs += sink((rei_any_fn) &rei_live_dir);
  refs += sink((rei_any_fn) &rei_live_open);
  refs += sink((rei_any_fn) &rei_live_try);
  refs += sink((rei_any_fn) &rei_live_close);
  refs += sink((rei_any_fn) &rei_preamble_write);
  refs += sink((rei_any_fn) &rei_preamble_validate);

  /* Pool map support and the unwind path. */
  refs += sink((rei_any_fn) &rei_pool_signals);
  refs += sink((rei_any_fn) &rei_pool_help_once);
  refs += sink((rei_any_fn) &rei_pool_deque_pull);
  refs += sink((rei_any_fn) &rei_pool_map_caps);
  refs += sink((rei_any_fn) &rei_pool_submit_flags);
  refs += sink((rei_any_fn) &rei_pool_eval_mark);
  refs += sink((rei_any_fn) &rei_pool_unwind_sink);

  /* Utilities. */
  refs += sink((rei_any_fn) &rei_now);
  refs += sink((rei_any_fn) &rei_self_pid);
  refs += sink((rei_any_fn) &rei_err_describe);
  refs += sink((rei_any_fn) &rei_rng_jump);
  refs += sink((rei_any_fn) &rei_tune);

  /* The dual-form accessors: the header's static inlines by default,
     the exported symbols under EXT_PROBE_EXPORTS. */
  refs += sink((rei_any_fn) &rei_parker_snapshot);
  refs += sink((rei_any_fn) &rei_zc_rc);
  refs += sink((rei_any_fn) &rei_zc_flags_);

  assert(refs == 44);

  /* Every ext-tier type is complete here (internal.h is absent). */
  size_t sizes = sizeof(rei_binding) + sizeof(rei_read_ctx) +
    sizeof(rei_result_sink) + sizeof(rei_shm) + sizeof(rei_parker) +
    sizeof(rei_pool_sig) + sizeof(rei_bytes);
  assert(sizes > 0);

  /* Macro surface. */
  assert(sizeof(REI_PREFIX_LITERAL) > 1);
  assert(REI_ALIGN64(1) == 64 && REI_ALIGN64(64) == 64 &&
         REI_ALIGN64(65) == 128);
  assert(REI_ZC_FLOOR < REI_ZC_FLOOR_RAW);
  assert(REI_OPEN_CACHE_MAX == 16);
  assert(REI_CODEC_MAGIC == 'R');
  assert(REI_HTYPE_CHANNEL != REI_HTYPE_POOL);
  assert(REI_PARK_WOKEN != REI_PARK_TIMEOUT);
  assert(REI_LIVE_ACQUIRED != REI_LIVE_HELD);

  /* rei_binding_init zeroes and size-stamps. */
  rei_binding b;
  memset(&b, 0xff, sizeof b);
  rei_binding_init(&b);
  assert(b.size == sizeof b && b.stage == NULL && b.read == NULL &&
         b.exec == NULL && b.ctx == NULL);

  /* The dual forms compute the wire-format offsets, in either mode. */
  unsigned char region[REI_HEADER_SIZE];
  memset(region, 0, sizeof region);
  atomic_store_explicit(rei_zc_rc(region), 7, memory_order_relaxed);
  atomic_store_explicit(rei_zc_flags_(region), 3, memory_order_relaxed);
  uint32_t rc_word;
  memcpy(&rc_word, region + REI_ZC_REFCOUNT_OFF, sizeof rc_word);
  assert(rc_word == 7);
  uint32_t fl_word;
  memcpy(&fl_word, region + REI_ZC_FLAGS_OFF, sizeof fl_word);
  assert(fl_word == 3);

  uint32_t epoch = 42;
  rei_parker pk;
  memset(&pk, 0, sizeof pk);
  pk.epoch = (_Atomic uint32_t *) &epoch;
  assert(rei_parker_snapshot(&pk) == 42);

  /* A taste of the stable tier: the proof links both headers' surface. */
  assert(rei_version() != NULL);
  assert(rei_now() > 0.0);
  assert(rei_self_pid() > 0);
  const char *summary, *hint;
  rei_err_describe(REI_ERRCAT_NOSPACE, &summary, &hint);
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
