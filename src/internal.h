/* internal.h — librei cross-TU internals

   Not installed, not FFI-stable, no ABI discipline: this header changes
   freely. -fvisibility=hidden keeps these symbols out of the shared
   library; the amalgamation folds it in.

   Consumers: the core's own TUs only. The binding-author surface is
   include/rei_ext.h (installed, version-pinned per minor release) — a
   binding never includes this header. The two headers share the
   offset-math accessors by inclusion: the dual-form static inlines used
   below come from rei_ext.h, so core and binding can never drift.

   Macros and static inline are fine here (no FFI to reach them). */

#ifndef REI_INTERNAL_H
#define REI_INTERNAL_H

#include <string.h>

#include "rei.h"
#include "rei_ext.h"

/* Some static inlines below go unused in some TUs; the amalgamation
   folds this header into the single rei.c TU, where they would trip
   -Wunused-function (header inlines are exempt only in a real header). */
#if defined(__GNUC__) || defined(__clang__)
#  define REI_MAYBE_UNUSED __attribute__((unused))
#else
#  define REI_MAYBE_UNUSED
#endif

// Region layer internals ---------------------------------------------------------

/* macOS registry dir: "<dir>/rei". The liveness dir env override: */
#define REI_LIVENESS_DIR_ENV "REI_LIVENESS_DIR"

/* Stack forms (the public API is the heap form). create returns an
   rei_errcat (REI_ERRCAT_NONE on success); open returns 0/-1. */
int rei_shm_create_stack(rei_shm *shm, size_t size);
int rei_shm_open_stack(rei_shm *shm, const char *name);
/* Heap forms without the error-slot record (the public verbs add it):
   create_heap returns an rei_errcat; open_heap returns NULL on failure. */
int rei_shm_create_heap(rei_shm **out, size_t size);
rei_shm *rei_shm_open_heap(const char *name);
/* Writable-attach stack form (shm_rw.c): both sides write the control
   regions, so peers map read-write; populate pre-faults. */
int rei_shm_open_rw_stack(rei_shm *shm, const char *name, int populate);
rei_shm *rei_shm_open_rw_heap(const char *name, int populate);
/* Unmaps only (never frees the struct); unlink != 0 also removes the
   name. The public rei_shm_close wraps this + free. */
void rei_shm_close_stack(rei_shm *shm, int unlink);

/* macOS registry-log exit/unload hook (shm.c; defined under __APPLE__
   only): removes this process's log and prunes the registry dir once
   every created region is torn down. Registered as a library
   destructor; declared here for the unit tier. */
void rei_log_teardown(void);

/* Control-region create: pre-faulted on every platform, so slot walks
   never zero-fill-fault on the hot path. Payload regions use the plain
   create — written in full at stage time. */
int rei_shm_create_populate(rei_shm *shm, size_t size);

/* SHM_RAW consumer open: read-only, populated on Linux (the stream is
   unserialized in full immediately; eager PTE install beats a fault per
   page). The rei_read_region cache rides this. NULL on failure. */
rei_shm *rei_shm_open_ro_heap(const char *name);

/* Host-side teardown of a created region: releases the SHM name (POSIX:
   unlink) / creator handle (Windows) without touching the mapping. */
void rei_shm_host_release(rei_shm *shm);
/* The survivor-unlink half of a control region's teardown (spill.c):
   releases the name / creator handle, keeping the mapping (the death
   watch and parkers reference it until the handle's release). */
void rei_region_unlink(rei_shm *shm);

/* Cold annotation for the error recorders: they sit on every hot verb's
   failure branches, and as returning variadic functions they otherwise
   force the compiler to treat those branches as live (register pressure,
   worse layout, blown inline budgets). */
#if defined(_MSC_VER)
#  define REI_COLD
#else
#  define REI_COLD __attribute__((cold))
#endif

// Errors ---------------------------------------------------------------------------

/* The handle error slot lives in rei_handle_s; handle-free entry points
   (regions, prune) use a thread-local slot. Both are printf-formatted
   records, valid until the next call on the same handle / thread. */
REI_COLD void rei_err_record(rei_handle *h, rei_errcat cat, const char *fmt, ...);
REI_COLD void rei_err_record_tls(rei_errcat cat, const char *fmt, ...);

// Spin machinery (parker.c) --------------------------------------------------------

/* Time-boxed pre-park spins: a park/wake round trip costs microseconds,
   so a wait that would park first spins against a nanosecond budget.
   Budgets adapt per handle (process-local, never shared): an empty spin
   halves toward REI_SPIN_FLOOR_NS; a completed wait learns its measured
   turnaround via rei_spin_learn (1.5x + headroom, capped). */
#define REI_SPIN_BUDGET_NS          16000
#define REI_COLLECT_SPIN_BUDGET_NS  32000
#define REI_SPIN_FLOOR_NS           1000
#define REI_SPIN_CAP_NS             64000

static inline REI_MAYBE_UNUSED uint64_t rei_spin_learn(double gap_ns, uint64_t budget,
                                      uint64_t base) {
  if (gap_ns <= (double) REI_SPIN_CAP_NS) {
    uint64_t b = (uint64_t) (1.5 * gap_ns) + 8000;
    if (b < base) b = base;
    return b > (uint64_t) REI_SPIN_CAP_NS ? (uint64_t) REI_SPIN_CAP_NS : b;
  }
  budget = budget / 2;
  return budget < REI_SPIN_FLOOR_NS ? REI_SPIN_FLOOR_NS : budget;
}

/* Clock reads per spin stride: default for O(n) scan predicates, LIGHT
   for 1-2-load predicates (a clock read measurably dominates those). */
#define REI_SPIN_CLOCK_EVERY        8
#define REI_SPIN_CLOCK_EVERY_LIGHT  64

/* Busy-path bound on result-keeper reap visits per worker step. */
#define REI_REAP_QUOTA 32

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
#  define REI_PAUSE() __builtin_ia32_pause()
#elif defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
#  define REI_PAUSE() __asm__ __volatile__("yield" ::: "memory")
#else
#  define REI_PAUSE() do { } while (0)
#endif

/* Time-boxed pause-hinted spin, the shared pre-announce wait layer:
   evaluate cond each iteration until it holds (out = 1) or the
   rei_now()-scale deadline `until` passes (out = 0), reading the clock
   every `stride` iterations. A macro: the predicate must inline at each
   site. Callers clamp `until` to any outer wait deadline themselves. */
#define REI_SPIN_WAIT(cond, until, stride, out)                         \
  do {                                                                  \
    (out) = 0;                                                          \
    for (;;) {                                                          \
      int rei_sw_i = 0;                                                 \
      for (; rei_sw_i < (stride); rei_sw_i++) {                         \
        REI_PAUSE();                                                    \
        if (cond) { (out) = 1; break; }                                 \
      }                                                                 \
      if ((out) || rei_now() >= (until)) break;                         \
    }                                                                   \
  } while (0)

// Parker internals -----------------------------------------------------------------

/* The parker struct, the park/unpark verbs, and the rei_parker_snapshot
   fast path are binding-author surface (rei_ext.h); the handshake
   contract is documented there. What stays here: the platform bounds. */

/* POSIX parks are always timed: an untimed wait is silently restarted
   under SA_RESTART and would swallow Ctrl-C until the next genuine wake,
   so "indefinite" (timeout_ms < 0) uses this nominal bound and relies on
   directed unparks. Windows uses INFINITE there. */
#define REI_PARK_NOMINAL_MS 3600000L

/* Interrupt-latency bound on parks from interactive processes. */
#ifdef _WIN32
#  define REI_INTERRUPT_BOUND_MS 100L
#else
#  define REI_INTERRUPT_BOUND_MS 2000L
#endif

// Death listener internals ---------------------------------------------------------

/* The rei_death_watch type, rei_death_watch_start/stop, and the
   listener teardown are binding-author surface (rei_ext.h). start2
   stays private: the callback-capable form is the pool's worker reap. */

/* As rei_death_watch_start, plus a callback invoked after the flag store
   and unpark, on the listener's callback thread (or synchronously from
   start when the pid is already dead): pure C only; its targets must
   stay valid until rei_death_watch_stop returns. The pool's worker reap
   rides this. This is the threading contract a binding's trace/eval
   hooks must respect — see the callback-threading note in rei.h. */
rei_death_watch *rei_death_watch_start2(long pid, _Atomic int *flag,
                                        const rei_parker *pk,
                                        void (*cb)(void *), void *cb_arg);

// Liveness lock internals (liveness.c) ------------------------------------------

/* The liveness contract, rei_live_dir/open/try/close, and the
   rei_live_probe enum are binding-author surface (rei_ext.h). What
   stays here: the prober's kept-fd forms. */

/* Open without creating: ENOENT reads as "indeterminate, treat as
   alive", never a verdict — the probe-by-path discipline. */
int rei_live_open_existing(const char *path, intptr_t *out);
/* Release an acquired lock while keeping the fd — the kept-fd prober's
   epilogue after a reap, so a respawned holder can lock the same file. */
void rei_live_unlock(intptr_t h);
/* The locked file's identity — (dev, inode) / (volume serial, file
   index) — recorded in registry slots at join so a path-opened prober
   can discard a probe whose file was unlinked and recreated. */
int rei_live_ident(intptr_t h, uint64_t *dev, uint64_t *ino);

// Preamble internals (preamble.c) --------------------------------------------------

/* rei_preamble_write/validate are binding-author surface (rei_ext.h).
   What stays here: the attach-path checks. */

/* The join-token charset check shared by the channel and pool attach
   paths ("<pid hex>_<counter hex>"; NULL and empty are malformed). */
int rei_token_valid(const char *token);
/* The pool header's magic + version + layout check (pool.c), the attach
   path's counterpart of rei_preamble_validate. */
const char *rei_pool_hdr_validate(const void *region, size_t region_size,
                                  rei_pool_hdr *out);

// Payload-policy helpers ------------------------------------------------------------

/* Whether a staged payload created no keeper record, so the collect-side
   keeper-drop wake is pure cost: the immediate kinds, or a self-contained
   codec stream inline (payload byte 0 is the codec magic — an INLINE
   stream is never empty). Core-only: the core owns the wake gate. The
   magic byte itself is binding-registry surface (rei_ext.h). */
static inline REI_MAYBE_UNUSED int rei_keeperless(uint32_t kind,
                                 const unsigned char *payload) {
  return kind == REI_KIND_NIL || kind == REI_KIND_RAWVEC ||
    kind == REI_KIND_STR1 ||
    (kind == REI_KIND_INLINE && payload[0] == REI_CODEC_MAGIC);
}

// Spill free list, lent-region ledger, open cache, retain table (spill.c) ---------

#define REI_SPILL_FL_MAX    16
#define REI_SPILL_FL_CLASS  2
#define REI_SPILL_FL_BYTES  ((size_t) 32 << 20)
#define REI_SPILL_FL_FLOOR  ((size_t) 4096)
#define REI_LEDGER_MAX      64

/* Producer spill-region free list + lent-region ledger. Regions are
   rei_shm pointers owned by the handle outright: created or popped at
   stage, surrendered at the consumer-done release points, closed +
   unlinked at eviction and teardown. Reuse is safe only because a region
   is offered exclusively at the protocol's consumer-done release points. */
typedef struct rei_spill_fl_s {
  rei_shm *regions[REI_SPILL_FL_MAX]; /* owned; evict/teardown = close+unlink */
  size_t   size[REI_SPILL_FL_MAX];    /* region size; 0 = empty entry */
  uint64_t stamp[REI_SPILL_FL_MAX];   /* push order: largest-oldest evict */
  uint64_t tick;
  size_t   total;
  uint32_t n;
  int      last_reused;               /* whether the last pop hit */
  uint64_t hits;                      /* process-local, dump-only */
  rei_shm *led_regions[REI_LEDGER_MAX];  /* lent: views outstanding */
  int32_t  led_key[REI_LEDGER_MAX];      /* consumer identity; -1 channel */
  uint32_t led_n;
  /* ledger-overflow drops: the mapping closes at the drop; the name is
     kept so a REF in flight can still resolve, and unlinks at teardown */
  rei_shm **dropped;
  uint32_t dropped_n;
  uint32_t dropped_cap;
  /* the one uncommitted staging checkout: set by rei_stage_spill_get,
     committed to a retain-table slot (or discarded) at publish, rolled
     back to the free list at the next staging verb or handle teardown —
     a mid-stage abandon never leaks a region. pin/kind ride along: a
     registered pin is dropped through the binding's drop hook on every
     abandonment path. */
  rei_shm *staging;
  void    *staging_pin;
  uint8_t  staging_kind;              /* REI_KEEP_SPILL / _ZC / _PIN */
  /* set when a spill pop misses with lent regions outstanding (the sweep
     just proved consumer-side views outlive their traffic): the signal
     for the binding's copy-tier fallback; cleared when a ledger sweep or
     force-reclaim returns a lent region. Raised on Linux only (fresh
     regions pre-fault there; macOS/Windows creates are lazy, so SHM_VEC
     stays cheaper than the copy tiers even under churn). */
  int churn;
} rei_spill_fl;

/* Per-slot retain table entry — the explicit successor of a GC keeper.
   Staging always copies, so the only retained objects are the regions a
   staged payload references plus the binding's opaque pin (the staged
   object, whose serialized stream may carry hook-emitted identifiers).
   kind: SPILL — the region surrenders to the free list at release; ZC —
   the producer-loan refcount sub, then free list or lent ledger by key;
   PIN — no region, the pin alone. The pin is dropped through the
   binding's drop hook at every release point. */
enum { REI_KEEP_FREE = 0, REI_KEEP_SPILL, REI_KEEP_ZC, REI_KEEP_PIN };

typedef struct rei_keeper_s {
  rei_shm *region;  /* SPILL/ZC: the staged region, owned until release */
  void    *pin;     /* opaque binding token, dropped via binding.drop */
  int32_t  key;     /* ZC: the consumer's identity for the lent ledger */
  uint8_t  kind;
} rei_keeper;

/* The retain-table primitives. h supplies the free list and the
   binding's drop hook; tab is one of the handle's tables (the pool has
   two). commit moves the current staging checkout into the (FREE) slot;
   release surrenders the slot's entry per its kind at a consumer-done
   point; discard abandons a never-published staged entry (a cancelled
   publish); rollback returns an uncommitted checkout to the free list;
   teardown closes + unlinks everything at handle death. */
void rei_keeper_commit(rei_handle *h, rei_keeper *tab, uint32_t at);
void rei_keeper_release(rei_handle *h, rei_keeper *tab, uint32_t at);
void rei_keeper_discard(rei_handle *h);
void rei_stage_rollback(rei_handle *h);
void rei_keepers_teardown(rei_handle *h, rei_keeper *tab, uint32_t n);
void rei_spill_fl_teardown(rei_spill_fl *fl);

/* Pop the smallest fitting free-list region (a full ledger sweep first on
   a miss) or create one fresh — at the pow2 size class when a free list
   is in play, exact otherwise. The checkout is recorded in fl->staging
   until committed or rolled back. NULL + the thread-local error slot on
   create failure. */
rei_shm *rei_spill_region_get(rei_spill_fl *fl, size_t n);
/* Insert a producer region into the free list under the size-class and
   byte caps (evicting largest-oldest), or close + unlink it in place
   when it doesn't fit. */
void rei_spill_fl_insert(rei_spill_fl *fl, rei_shm *shm);
/* The producer-loan release for a ZC retain entry: refcount sub, then
   free list on 0 or the lent-region ledger otherwise. */
void rei_zc_release(rei_spill_fl *fl, rei_shm *shm, int32_t key);
/* Move zero-count ledger entries to the free list, up to quota
   (REI_LEDGER_MAX = full sweep). */
void rei_ledger_sweep(rei_spill_fl *fl, uint32_t quota);
/* Force-reclaim ledger entries after a confirmed consumer death: key >= 0
   matches that consumer only, key < 0 all entries (the channel's single
   peer). REFHELD entries leak + unlink; the rest rejoin the free list. */
void rei_ledger_force(rei_spill_fl *fl, int32_t key);

/* Consumer-side SHM_RAW mapping cache: a name -> mapping table that
   skips the open/fstat/mmap per payload once producers repeat region
   names. Mappings are handle-owned — eviction and teardown close them in
   place; the SHM_RAW tiers copy out before consumer-done, so no view
   outlives a mapping here (view caches are binding-owned for that
   reason). Names never alias and a region's size is fixed for its
   lifetime, so entries cannot go stale. Counters are process-local,
   dump-only. */
typedef struct rei_open_cache_s {
  rei_shm *maps[REI_OPEN_CACHE_MAX];  /* owned; evict = close */
  char names[REI_OPEN_CACHE_MAX][REI_NAME_MAX];
  uint8_t name_len[REI_OPEN_CACHE_MAX];   /* 0 = empty entry */
  uint64_t stamp[REI_OPEN_CACHE_MAX];
  uint64_t tick;
  uint64_t hits, misses;
} rei_open_cache;

rei_shm *rei_oc_lookup(rei_open_cache *oc, const unsigned char *name,
                       uint32_t len);
void rei_oc_store(rei_open_cache *oc, rei_shm *shm);
void rei_oc_teardown(rei_open_cache *oc);

// Handle base (channel.c / pool.c embed as first member) --------------------------

/* Every handle embeds this header as its first member, so an
   rei_channel or rei_pool pointer converts to rei_handle for the
   callback seam. The full struct definitions live with their TUs.
   htype (REI_HTYPE_*, rei_ext.h) dispatches the handle-kind-specific
   staging services (rei_stage_arena_alloc / rei_stage_reap are
   channel-only). */
struct rei_handle_s {
  rei_binding binding;          /* copied in at create/attach: one load +
                                   a predicted indirect branch per call */
  rei_errcat errcat;            /* verb error slot — valid until the next */
  char errmsg[256];             /*   call on this handle */
  uint8_t htype;                /* REI_HTYPE_*; 0 = unset */
  rei_spill_fl fl;              /* producer free list + lent-region ledger */
  rei_open_cache oc;            /* consumer SHM_RAW mapping cache */
  rei_read_ctx read_tmpl;       /* per-read ctx template: size/handle/
                                   binding_ctx set once at create/attach;
                                   each read copies it and fills only the
                                   outcome fields. Handles are
                                   single-threaded per side, so a
                                   handle-owned mutable ctx is safe. */
};

/* Initialize the handle's read-ctx template once the binding is in
   (create/attach/join). */
static inline REI_MAYBE_UNUSED void rei_read_tmpl_init(rei_handle *h) {
  memset(&h->read_tmpl, 0, sizeof(h->read_tmpl));
  h->read_tmpl.size = (uint32_t) sizeof(h->read_tmpl);
  h->read_tmpl.died_slot = -1;
  h->read_tmpl.handle = h;
  h->read_tmpl.binding_ctx = h->binding.ctx;
}

static inline REI_MAYBE_UNUSED int rei_check_interrupt(const rei_binding *b) {
  return b->check != NULL ? b->check(b->ctx) : 0;
}

static inline REI_MAYBE_UNUSED void rei_park_bracket(const rei_binding *b, int entering) {
  if (b->park != NULL) b->park(b->ctx, entering);
}

#endif /* REI_INTERNAL_H */
