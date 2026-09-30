/* mizu_ext.h — libmizu binding-author API (the unstable tier)

   The sanctioned surface for language bindings and the built-in bytes
   binding: the callback seam, the stager/read/publish services, and the
   promoted internals the first-party bindings (mizu for R, pymizu for
   Python) compile against.

   Tier policy: this header is version-pinned per minor release. Any
   declaration, struct layout, or macro here may change on a minor
   version bump, without deprecation — including after mizu.h's stable
   promise starts at 1.0. Consumers rebuild against each libmizu minor
   release (the first-party bindings vendor the sources at a pinned
   commit). mizu.h carries the stable-ABI promise; src/internal.h stays
   private. There is no third state: a symbol not here or in mizu.h is
   private.

   Conventions differ from mizu.h where the consumers differ: this tier
   is C-only binding surface, so macros are fine, and the hot accessors
   plus the small wire-format helpers ship in dual form (below). Every
   other operation is a real exported function (MIZU_API), so FFI
   consumers see the full tier.

   Dual-form fast paths (the CPython Py_INCREF pattern):
   mizu_parker_snapshot, mizu_zc_rc, mizu_zc_flags_, the wire helpers
   (mizu_timeout_ms, mizu_store_na_real, mizu_aux_rawspill_pool,
   mizu_aux_shm_vec, mizu_mizh_write, mizu_mizh_check,
   mizu_mizh_validity_set, mizu_mizs_geometry, mizu_mizs_check,
   mizu_mizl_check, mizu_mizl_elem, mizu_na_build, mizu_na_apply), and the
   interchange emit helpers (mizu_ix_put_*) ship as
   `static inline` here AND as same-named exported functions (src/ext.c).
   A C TU inlines its own copy — zero cost — while an FFI binds the
   exported symbol. This is legal C: internal and external linkage of the
   same name in different TUs do not clash; taking the address in an
   ordinary TU yields the local inline's address, which is harmless (the
   accessors are pure offset math over the wire-format offsets). A TU
   defining the exported forms defines MIZU_EXT_NO_INLINES before including
   this header: a static inline and an extern definition of one name
   cannot coexist in a TU. Single-sourcing: both forms are thin wrappers
   over the same offset macros / struct definitions below — the macros,
   not the bodies, are the one definition. */

#ifndef MIZU_EXT_H
#define MIZU_EXT_H

#include <math.h>
#include <string.h>

#include "mizu.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Some static inlines below go unused in some TUs; the amalgamation
   folds this header into the single mizu.c TU, where they would trip
   -Wunused-function (header inlines are exempt only in a real header). */
#if defined(__GNUC__) || defined(__clang__)
#  define MIZU_EXT_INLINE static inline __attribute__((unused))
#else
#  define MIZU_EXT_INLINE static inline
#endif

/* The dual-form inlines touch atomics; spell the acquire load per
   language (mizu.h's MIZU_ATOMIC gives C++ consumers std::atomic). */
#ifdef __cplusplus
#  define MIZU_EXT_LOAD_ACQ(p) \
     std::atomic_load_explicit((p), std::memory_order_acquire)
#else
#  define MIZU_EXT_LOAD_ACQ(p) \
     atomic_load_explicit((p), memory_order_acquire)
#endif

// Binding-tier constants ---------------------------------------------------------

#ifdef _WIN32
#  define MIZU_PREFIX_LITERAL "Local\\mizu_"
#else
#  define MIZU_PREFIX_LITERAL "/mizu_"
#endif

#define MIZU_ALIGN64(x) (((x) + 63) & ~(size_t) 63)

/* Staging-policy floors for a binding's stage_fn (not used by the
   core): SHM_VEC escalates only past max(inline budget, MIZU_ZC_FLOOR);
   the channel's raw floor is higher (the arena copy has no region
   machinery to amortize) and lifts entirely under the churn signal.
   Bindings pick their own. */
#define MIZU_ZC_FLOOR     ((size_t) 32768)
#define MIZU_ZC_FLOOR_RAW ((size_t) (256 << 10))

/* The core's consumer SHM_RAW mapping cache size; a binding sizes its
   own view cache to match. */
#define MIZU_OPEN_CACHE_MAX  16

/* The self-describing stream dispatch byte: payload byte 0 of an INLINE
   frame, where an R native stream carries 'B'/'X'. The core is
   codec-agnostic; DESIGN.md's codec registry allocates the magic bytes
   ('R' = mizu, 'P' = pymizu). 'R' here and MIZU_DROP_R share the letter
   deliberately — both denote an R-binding payload, in disjoint
   contexts. */
#define MIZU_CODEC_MAGIC 0x52u   /* 'R' */
#define MIZU_PYMIZU_CODEC_MAGIC 0x50u   /* 'P' */
#define MIZU_INTEROP_MAGIC 0x49u   /* 'I': the interchange stream (DESIGN.md's
                                      Interchange codec section) */

// Language and capability registries ----------------------------------------------

/* The language registry: the identity word's byte 0, the pool's worker
   identity word, and each binding's handle-level peer_lang. Append-only:
   a value, once shipped, is never reassigned (the byte is
   equality-compared, so a reassignment fails behaviorally, as false join
   rejections across mixed builds). 0 is "none" — no peer attached yet, a
   pool no worker has joined — never a stored binding identity: a binding
   whose identity word has a zero language byte is rejected at create,
   attach and join. */
#define MIZU_LANG_NONE   0u
#define MIZU_LANG_BYTES  1u   /* the core's bytes binding and test bindings */
#define MIZU_LANG_R      2u
#define MIZU_LANG_PYTHON 3u

/* Reader capabilities: a 32-bit mask, one bit per layout or format
   extension a reader implements beyond the baseline (MIZH atomic and
   INT64 views, 'I' format 0x01 as specified in DESIGN.md's Interchange
   codec section). Readers ignore bits they do not know — they never
   reject them; absent means unsupported, and a writer stages a layout or
   tag only for a peer that sets its bit. A bit names a byte layout, not a
   feature: an incompatible change to a gated layout, tag or shape
   allocates a new bit and retires the old, which is never reassigned.
   Features that always land together in every binding share a bit. */
#define MIZU_CAP_MIZS  (1u << 0)   /* reads MIZS string layouts */
#define MIZU_CAP_ATTRS (1u << 1)   /* reads an 'I' attribute blob on a
                                      layout root or MIZL leaf */
#define MIZU_CAP_MIZL  (1u << 2)   /* wraps a generic MIZL tree as views */

/* The identity word: the language in bits 0-7, the 32-bit capability mask
   in bits 32-63, bits 8-31 reserved — written zero and ignored by readers
   (a negotiation word, so a later field needs no reader taught to skip
   it). The pool word's exact-match join compares the whole word. */
#define MIZU_IDENT(lang, caps) \
  ((uint64_t) (uint8_t) (lang) | ((uint64_t) (uint32_t) (caps) << 32))

// Handle views -------------------------------------------------------------------

/* Generic handle view for the stager services; the core passes the
   handle a binding's callbacks were registered with. Never constructed
   by bindings; the struct definition stays private (src/internal.h). */
typedef struct mizu_handle_s mizu_handle;
/* Valid only during an exec callback. The struct definition is below
   (the ERR-envelope frame buffer is binding surface). */
typedef struct mizu_result_sink_s mizu_result_sink;

/* Handle-kind discriminator. The staging services are kind-specific:
   mizu_stage_arena_alloc / mizu_stage_reap are channel-only. */
enum { MIZU_HTYPE_CHANNEL = 1, MIZU_HTYPE_POOL = 2 };

/* Handle queries standing in for field access into the private handle
   struct. kind returns MIZU_HTYPE_*. churn reads the spill-churn signal
   (raised on Linux only: a spill pop missed with lent regions
   outstanding; cleared when a ledger sweep or force-reclaim returns one)
   — while set, a stager falls back to the copy tiers. The churn read is
   an extern call: gate it behind the payload's size check so it runs
   only for payloads already proven large. spill_info fills the producer
   free-list entry count and the lent-region ledger count (debug/dump
   surface; the per-kind public snapshots are mizu_channel_info_get /
   mizu_pool_dump_get). */
MIZU_API int mizu_handle_kind(const mizu_handle *);
MIZU_API int mizu_handle_churn(const mizu_handle *);
/* The tx-keeper count: outstanding staging retains on a channel handle
   (the reap gate's own count, so side-effect free). -1 for a pool
   handle (its keepers live on both sides of every queue; no one count). */
MIZU_API int64_t mizu_handle_keep_out(const mizu_handle *);
MIZU_API void mizu_handle_spill_info(const mizu_handle *,
                                   uint32_t *fl_entries,
                                   uint32_t *ledger_entries);

// The binding seam ---------------------------------------------------------------

/* The core never sees a language object. A binding registers these
   callbacks at create/attach/join; the core copies them into the handle,
   so a hot-path call is one load + a predicted indirect branch.

   stage: frame obj as (hdr, payload) — payload capacity inline_max.
     Spill/arena/retain via the mizu_stage_* services on the handle. ctx
     is the handle's binding.ctx. Returns 0 on success, nonzero on
     staging failure (the verb fails as MIZU_ERR / MIZU_ERRCAT_STAGE). May
     also not return (a binding's longjmp):
     staging is transactional — the core mutates no shared state before
     stage returns, a mid-stage arena chunk is FIFO-reclaimed like any
     other, and an uncommitted region checkout or pin rolls back at the
     next verb entry or at destroy. An abandoned stage leaves the handle
     consistent.
   read: produce the binding's object for a received frame; the return
     value goes to the verb's caller, opaque to the core. May also not
     return (a binding's longjmp): invocation points leave the handle
     consistent and nothing consumed. payload is
     always a dereferenceable byte range: the transport resolves its
     arena-referencing kinds (ARENA, channel RAWSPILL) to their byte
     range before the call, bounds-checked against the arena, passing
     the validated capacity as limit (inline_max for slot-resident
     frames). The kinds are NOT rewritten. For a pool collect,
     ctx->outcome carries the result's terminal state (MIZU_RS_OK/ERR
     with a payload frame; MIZU_RS_CANCEL/DIED without one — read_fn
     builds the binding's error object for all three). A vanished
     out-of-line region: mizu_read_region sets ctx->gone and read_fn
     propagates by returning NULL. A NULL return consumes nothing —
     unless the read first set ctx->flags |= MIZU_READ_CONSUME: then the
     transport consumes exactly as on success (channel: the head advance
     and its batched publication; pool OK/ERR: the FREE transition, the
     task-keeper drop, and the producer-keeper wake) while the verb
     still returns MIZU_ERR. Use for foreign or corrupt payloads that
     must not wedge the ring behind an unreadable slot: the binding
     carries its specific message in its own state (the core records no
     generic error for a consumed read), and a retried read sees the
     next slot. The consume decision sits with the binding on a single
     receive and on a batch's first message; past that the core defers
     it — a mid-batch read failure ends the batch with MIZU_OK and the
     consumed prefix, and the next receive reproduces the failure and
     decides.
   exec: pool workers only — run one claimed task frame and publish
     through the sink. ctx is a read ctx on the handle (outcome
     MIZU_RS_OK): decode reads ride it (mizu_read_region, ctx->gone), and
     the binding ctx remains reachable as ctx->binding_ctx. Must not
     abandon: the binding catches every task
     condition into the sink; an escape degrades to worker death plus
     the reaper verdict (the hard-crash semantics, never the path for an
     ordinary task error). catching marks a reentrant invocation
     (nested-collect help, nested submit's inline execute): contain task
     conditions there instead of letting them unwind through the worker
     loop. Returns 0 on success; nonzero is infrastructure failure and
     takes the worker down (MIZU_EXIT_ERROR).
   check: interrupt/cancel poll, invoked from core wait/work loops at
     abandon-safe points only (no shared-state mutation in progress, no
     cleanup pending), once per spin/park iteration. Return 0 to
     continue, nonzero to abandon: the verb unwinds as MIZU_ERR with
     MIZU_ERRCAT_INTERRUPTED, consuming nothing (a recv interrupted
     mid-wait has claimed no slot; a collect interrupted while parked
     has consumed no result). The hook may also not return (a binding's
     longjmp) — invocation points are chosen so an abandoned wait leaves
     the handle consistent. Verb-calling thread only, never death-watch
     or reaper threads. NULL for plain-C consumers: the poll skips at
     one load + a predicted branch.
   park: around-park hook, bracketing each bounded park — after the
     parker's announce, around the sleep only, never around shared-state
     mutation: `entering` is nonzero before the sleep, zero after the
     wake. For runtimes with a global lock: a Python worker holds the
     GIL through collect (nested-collect help reenters exec_fn); this
     hook drops it for each bounded sleep so other threads in the worker
     process run while the worker parks. Verb-calling thread only. NULL
     for runtimes without a global lock, plain-C consumers, and
     submitter handles (which release around the whole verb instead).
   sweep: idle hook, invoked when a pool worker goes idle or departs
     (the core's keeper-table sweep points): drop binding-side caches.
     NULL for bindings without per-handle caches.
   drop: pin-release hook, invoked when a retained staging entry is
     released — at the consumer-done points (collect, slot reuse, the
     worker keeper sweep), on a cancelled publish, on a staging
     rollback, and at handle teardown. `pin` is the opaque token the
     stager registered with mizu_stage_pin. Fires only on the
     handle-owning thread. NULL when the binding never pins.
   ctx: opaque to the core. */

typedef int (*mizu_stage_fn)(void *obj, mizu_slot_hdr *hdr,
                            uint8_t *payload, uint32_t inline_max,
                            mizu_handle *, void *ctx);

/* read_fn flags (ctx->flags, zero at each read's start): CONSUME makes a
   NULL return consume the slot exactly as on success while the verb
   still returns MIZU_ERR — the foreign/corrupt-payload contract; see the
   seam comment above. The flag is honoured on a single receive and on a
   batch's first message only; past that the core defers the consume
   decision to the next receive. */
#define MIZU_READ_CONSUME 1u

typedef struct mizu_read_ctx_s {
  uint32_t size;        /* core-set: sizeof the struct it knows */
  int32_t outcome;      /* pool: mizu_rs_status of the result (channel:
                           MIZU_RS_OK) */
  int32_t gone;         /* set by mizu_read_region on a vanished region;
                           read_fn propagates by returning NULL */
  int32_t died_slot;    /* MIZU_RS_DIED: the claimant worker slot (-1) */
  int64_t died_pid;     /* MIZU_RS_DIED: its pid (0 when unknown) */
  mizu_handle *handle;   /* the reading handle */
  void   *binding_ctx;  /* the handle's binding.ctx */
  uint32_t flags;       /* binding-set MIZU_READ_*; zero on entry */
  void   *reserved[4];  /* zero; future growth without a soname bump */
} mizu_read_ctx;

typedef void *(*mizu_read_fn)(const mizu_slot_hdr *, const uint8_t *payload,
                             size_t limit, mizu_read_ctx *);

typedef int (*mizu_exec_fn)(const mizu_slot_hdr *hdr, const uint8_t *payload,
                           size_t limit, mizu_result_sink *, int catching,
                           mizu_read_ctx *ctx);
typedef int (*mizu_check_fn)(void *ctx);
typedef void (*mizu_park_fn)(void *ctx, int entering);
typedef void (*mizu_sweep_fn)(void *ctx);
typedef void (*mizu_drop_fn)(void *ctx, void *pin);

typedef struct mizu_binding_s {
  uint32_t     size;    /* sizeof(mizu_binding); set via mizu_binding_init */
  mizu_stage_fn stage;
  mizu_read_fn  read;
  mizu_exec_fn  exec;    /* pool workers only; NULL on submitter handles */
  mizu_check_fn check;   /* interrupt poll; NULL for plain-C consumers */
  mizu_park_fn  park;    /* around-park lock release; usually NULL */
  mizu_sweep_fn sweep;   /* idle cache drop; usually NULL */
  mizu_drop_fn  drop;    /* pin release; NULL when the binding never pins */
  void        *ctx;     /* opaque to the core */
  uint64_t     ident;   /* MIZU_IDENT word; a zero language byte is
                           rejected at create, attach and join */
} mizu_binding;

/* Zero and size-stamp a binding struct. Call before filling the fn pointers. */
MIZU_API void mizu_binding_init(mizu_binding *);

/* The identity words (the language registry above). Each channel side
   publishes its binding's ident in its entity block: the host at create,
   the peer at attach before ready_set. mizu_channel_peer_ident reads the
   other side's word through the mapping (0 until the peer attaches: a
   host knows its peer's identity once ready_wait returns). A pool's
   first worker join CASes its word into the pool header — an exact match
   joins, a differing word fails — and mizu_pool_worker_ident reads that
   word through the mapping (0 until the first join; never reset for the
   pool's lifetime). */
MIZU_API uint64_t mizu_channel_peer_ident(const mizu_channel *);
MIZU_API uint64_t mizu_pool_worker_ident(const mizu_pool *);

/* Core services for a stager (per handle), valid only during a stage_fn
   call — staging is single-threaded per handle role, so at most one
   checkout is in flight per handle:
   - arena_alloc: reserve n bytes in the channel's spill arena; *off
     receives the chunk offset for the frame's aux. NULL when
     full/disabled. The arena base stays core-private.
   - spill_get: check out a region of at least n bytes — recycled from
     the free list or created fresh. Uncommitted until retained (below)
     and the frame publishes: rolls back to the free list at the next
     verb entry or at destroy, so a mid-stage abandon never leaks.
   - retain / retain_zc: commit the checkout's kind — SPILL surrenders
     to the free list at the consumer-done release point; ZC runs the
     zero-copy refcount protocol (the producer-loan store happens here)
     and lends to the ledger while views are outstanding. `region` must
     be the current checkout.
   - pin: attach an opaque token to the staging entry, handed to the
     drop hook at the entry's release (a binding pins the staged object,
     whose serialized stream may carry hook-emitted identifiers).
   - reap: the channel's pre-spill consumer-done reap, so a checkout
     sees the freshest surrenders. A no-op on pool handles. */
MIZU_API void *mizu_stage_arena_alloc(mizu_handle *, size_t n, uint64_t *off);
MIZU_API mizu_status mizu_stage_spill_get(mizu_handle *, size_t n,
                                       mizu_shm **out);
MIZU_API void mizu_stage_retain(mizu_handle *, mizu_shm *region);
MIZU_API void mizu_stage_retain_zc(mizu_handle *, mizu_shm *region);
MIZU_API void mizu_stage_pin(mizu_handle *, void *pin);
MIZU_API void mizu_stage_reap(mizu_handle *);

/* Raw-tier reservation (stage_raw.c), valid only during stage_fn: choose
   the tier for n bare bytes of wire_type and return the destination with
   hdr stamped — the caller memcpys n bytes into it. NULL hands the object
   to the binding's serialized tiers (a reservation failure degrades, never
   errors). Policy:
   n <= inline_max → RAWVEC inline (aux = wire_type).
   channel: arena RAWSPILL (aux = wire_type, payload = chunk offset) when
     n <= MIZU_ZC_FLOOR_RAW or under churn; else flat SHM_VEC (MIZH header +
     retain_zc, aux = wire_type | exact used bytes << 8, payload = region
     name) when n >= max(inline_max, MIZU_ZC_FLOOR) and no churn, reaping
     before the checkout; an arena retry on region failure; else NULL.
   pool: flat SHM_VEC on the same zc gate; else a RAWSPILL region (aux =
     wire_type | name_len << 8, payload = name; n <= UINT32_MAX); else NULL.
   Bare bytes carry no identifier, so nothing is pinned. Eligibility probes
   (which objects are raw), STR1/NIL/REF, and the string/list-tree SHM_VEC
   layouts stay binding-side. Dual form (see the banner): the inline stamps
   the RAWVEC fast path and falls through to the exported slow path. */
MIZU_API void *mizu_stage_raw_spill(mizu_handle *, uint64_t n, int wire_type,
                                  mizu_slot_hdr *, uint8_t *payload,
                                  uint32_t inline_max);
#ifdef MIZU_EXT_NO_INLINES
MIZU_API void *mizu_stage_raw(mizu_handle *, uint64_t n, int wire_type,
                            mizu_slot_hdr *, uint8_t *payload,
                            uint32_t inline_max);
#else
MIZU_EXT_INLINE void *mizu_stage_raw(mizu_handle *h, uint64_t n, int wire_type,
                                   mizu_slot_hdr *hdr, uint8_t *payload,
                                   uint32_t inline_max) {
  if (n <= (uint64_t) inline_max) {
    hdr->kind = MIZU_KIND_RAWVEC;
    hdr->len = (uint32_t) n;
    hdr->aux = (uint64_t) (uint32_t) wire_type;
    return payload;
  }
  return mizu_stage_raw_spill(h, n, wire_type, hdr, payload, inline_max);
}
#endif

/* Read-side service, invoked through the read_ctx handed to read_fn: a
   borrowed consumer mapping for a SHM_RAW-class payload name, from the
   handle's open cache (open/fstat/mmap on a miss, LRU-evicted). The
   mapping is cache-owned: copy the payload out before consumer-done. On
   a vanished region, sets ctx->gone and returns NULL; read_fn
   propagates by returning NULL. View tiers do not use it — their
   mappings are binding-owned (mizu_shm_open_view), pinned by the view. */
MIZU_API mizu_shm *mizu_read_region(mizu_read_ctx *, const uint8_t *name,
                                 uint32_t len);

/* Result publish, valid during exec only. publish stages value via the
   worker handle's stage_fn and publishes MIZU_RS_OK — a result past the
   inline budget spills to a region whose consumer is the submitter, so
   a large result never has to fit the slot. publish_err publishes
   MIZU_RS_ERR: when inline_n != 0 the binding has already framed the
   flattened envelope INLINE in the sink's frame buffer (a self-contained
   stream pins nothing); otherwise the flattened object rides the tiered
   stage (the tiny-slot fallback). The ERR envelope is INLINE-framed by
   construction wherever a classed condition fits the slot, so the
   publish cannot fail: a task error fails the task, never the worker.
   publish_died is the status-only terminal for a task whose out-of-line
   payload vanished with its dead enqueuer. The publish pair return 1 when
   the publish CAS won, 0 when a cancel beat it, and -1 on infrastructure
   failure (recorded on the handle — the worker cannot continue). */
MIZU_API int mizu_result_publish(mizu_result_sink *, void *value);
MIZU_API int mizu_result_publish_err(mizu_result_sink *, void *flattened,
                                   uint32_t inline_n);
MIZU_API void mizu_result_publish_died(mizu_result_sink *);

// The built-in bytes binding -----------------------------------------------------

/* A byte buffer. Send: the consumer fills {data, len} and passes its
   address as the verb's obj. Recv: the binding returns a malloc'd
   mizu_bytes (data rides the same allocation); release with
   mizu_bytes_free — an export, not documented free(), so a shared-library
   build never crosses allocator domains. Fixed layout, never extended. */
typedef struct mizu_bytes_s {
  void *data;
  size_t len;
} mizu_bytes;

/* Fill `binding` with the bytes binding: stage/read only — exec, check,
   park, sweep, drop are NULL (a bytes handle is a channel peer or a pool
   submitter, never a worker). Stage rides the INLINE/ARENA/SHM_RAW tiers
   exactly as a serialize stream does; read copies out before
   consumer-done. The copy tiers of a foreign peer (RAWVEC/RAWSPILL/STR1)
   read as their bare bytes; the view tiers (SHM_VEC/REF) fail the read —
   they are binding-owned by design. Pool collects surface non-OK
   outcomes only as the verb's mizu_status — the binding builds no error
   object. This is the FFI zero-callback path, the reference stager for
   binding authors, and the C test tiers' stager. */
MIZU_API void mizu_binding_bytes(mizu_binding *);
MIZU_API void mizu_bytes_free(mizu_bytes *);

// The region handle struct ---------------------------------------------------------

/* The public opaque type in mizu.h; defined here for bindings (the
   core's control-region mappings embed one in the handle structs).
   Layout-pinned per minor release like the rest of this tier: any field
   addition or reorder forces a binding rebuild. addr/size/name are
   borrowed reads, valid until close. */
struct mizu_shm_s {
  void *addr;
  size_t size;
  char name[MIZU_NAME_MAX];
  uint8_t name_len;
  unsigned int pid;            /* creator PID: fork guard (POSIX only) */
#ifdef _WIN32
  void *handle;
#endif
};

// Parker (wait_*.c) ---------------------------------------------------------------

/* One parker per waiting entity: a 32-bit monotonic epoch word in the
   shared region plus, on Windows only, one named auto-reset event (the
   epoch compare is not atomic with the sleep there). Park sites follow
   snapshot -> announce -> re-check -> sleep-bounded; any unpark that
   observes the announcement bumps the epoch after the snapshot, so the
   sleep returns immediately. Spurious wakes are absorbed by the caller's
   re-check. This handshake is the sole guarantee against lost wakeups —
   no watchdog sits behind it. */

typedef struct mizu_parker_s {
  MIZU_ATOMIC(uint32_t) *epoch;   /* in the shared region */
#ifdef _WIN32
  void *event;                   /* named auto-reset event handle */
#endif
} mizu_parker;

typedef enum mizu_park_result_e { MIZU_PARK_WOKEN = 0, MIZU_PARK_TIMEOUT,
                                 MIZU_PARK_INTR } mizu_park_result;

/* region_name/entity name the Windows event ("<region>.pk.<entity>"),
   created by the region's host (create = 1) and opened by name by
   attachers; unused on POSIX. Returns 0 on success. */
MIZU_API int mizu_parker_attach(mizu_parker *pk, MIZU_ATOMIC(uint32_t) *epoch,
                              const char *region_name, int entity,
                              int create);
MIZU_API void mizu_parker_detach(mizu_parker *pk);

/* Sleeps while the epoch still equals snapshot, up to timeout_ms
   (0 = poll: never sleeps; < 0 = indefinite — on POSIX an untimed wait
   would silently restart under SA_RESTART and swallow an interrupt until
   the next genuine wake, so indefinite parks are internally bounded and
   rely on directed unparks). Returns MIZU_PARK_*. */
MIZU_API int mizu_park(mizu_parker *pk, uint32_t snapshot, long timeout_ms);
MIZU_API void mizu_unpark(mizu_parker *pk);

#ifdef MIZU_EXT_NO_INLINES
MIZU_API uint32_t mizu_parker_snapshot(const mizu_parker *pk);
#else
/* The park handshake's first step (dual form — see the banner). */
MIZU_EXT_INLINE uint32_t mizu_parker_snapshot(const mizu_parker *pk) {
  return MIZU_EXT_LOAD_ACQ(pk->epoch);
}
#endif

// Death watch (wait_linux.c / wait_macos.c / wait_win32.c) ------------------------

/* Translates a watched pid's exit into *flag = 1 plus a directed unpark
   of pk (optional, copied). A pid already dead fires immediately. The
   flag target and the parker's epoch word / event must stay valid until
   mizu_death_watch_stop returns: stop synchronizes with any in-flight
   callback, so after it returns nothing touches them. Detection is a
   wake trigger only — the liveness lock is the verdict; pid-reuse races
   are absorbed there. Callback threading: the core's own reap watch
   (internal start2) fires on OS listener threads; start's flag store +
   unpark is the whole contract here. */

typedef struct mizu_death_watch_s mizu_death_watch;

MIZU_API mizu_death_watch *mizu_death_watch_start(long pid,
                                               MIZU_ATOMIC(int) *flag,
                                               const mizu_parker *pk);
MIZU_API void mizu_death_watch_stop(mizu_death_watch *w);

/* Library-unload teardown; joins the Linux epoll thread (no-op
   elsewhere: macOS dispatch sources and Windows thread-pool waits are
   per-watch). */
MIZU_API void mizu_death_listener_teardown(void);

// Liveness lock (liveness.c) ---------------------------------------------------

/* Exclusive flock (POSIX) / LockFileEx (Windows) held for a process's
   entire lifetime and released by the kernel on any exit path.
   fd-scoped, not PID-scoped: pid reuse cannot fake "alive". A probe is a
   non-blocking acquire on the fd kept from open — ACQUIRED means the
   previous holder is dead (and the caller now holds the lock,
   serializing survivor cleanup); HELD means alive. */

typedef enum mizu_live_probe_e { MIZU_LIVE_ACQUIRED = 0,
                                MIZU_LIVE_HELD = 1 } mizu_live_probe;

/* Directory for liveness lock files: the MIZU_LIVENESS_DIR override
   (read-through, checked every call) else a per-platform default
   resolved once — /dev/shm on Linux, the per-user temp dir on macOS and
   Windows. NULL if unresolvable. Only region creators call this;
   participants read the embedded copy. */
MIZU_API const char *mizu_live_dir(void);

MIZU_API int mizu_live_open(const char *path, intptr_t *out);
MIZU_API int mizu_live_try(intptr_t h);
MIZU_API void mizu_live_close(intptr_t h);

// Preamble (preamble.c) ----------------------------------------------------------

/* Host writes at create, immutable thereafter; the peer validates before
   any shared atomic is read or written. validate returns NULL and fills
   *out on success, else a static error message. The pool header check
   (magic + version) lives in the pool attach path, not here. */
MIZU_API void mizu_preamble_write(void *region, const mizu_preamble *p);
MIZU_API const char *mizu_preamble_validate(const void *region,
                                          size_t region_size,
                                          mizu_preamble *out);

// Map support (pool.c; a binding's map rides these) ---------------------------------

/* The opaque pool-signal trio a map runner loads relaxed once per batch
   transition: the help_wanted doorbell, the pool's shared shutdown word,
   and the handle's process-local listener-written owner_dead flag.
   Borrowed; the binding's map dereferences the words and stays
   pool-layout-free. */
typedef struct mizu_pool_sig_s {
  MIZU_ATOMIC(uint32_t) *help_wanted;
  MIZU_ATOMIC(uint32_t) *shutdown;
  MIZU_ATOMIC(int)      *owner_dead;
} mizu_pool_sig;

/* A malloc'd copy of the signal trio (the caller frees). NULL on
   failure. */
MIZU_API mizu_pool_sig *mizu_pool_signals(mizu_pool *);
/* One doorbell help beat (claims a map runner and re-homes it onto the
   helper's own deque) and the test-harness injection pull. help_once
   returns 1 when it claimed, 0 when not, -1 on an exec_fn
   infrastructure failure (recorded on the handle). */
MIZU_API int mizu_pool_help_once(mizu_pool *);
MIZU_API int mizu_pool_deque_pull(mizu_pool *, uint32_t n);
/* A map's batch-sizing inputs: claims the worker's nested-submitter slot
   when unclaimed (as a first nested submit does), then reports the
   caller's FREE result slots, the injection cap, and the entry inline
   budget. Only this process allocates from its own subrange, so the
   FREE count can only grow under it. Returns -1 on a claim failure (the
   handle error slot holds it). */
MIZU_API int mizu_pool_map_caps(mizu_pool *, uint32_t *free_rs,
                              uint32_t *inj_cap, uint32_t *inline_entry);
/* The map runner's submit: mizu_pool_submit plus the entry flags
   (MIZU_ENTRY_RUNNER marks a map's join ticket — a doorbell help beat
   re-homes it onto the helper's own deque instead of executing it). */
MIZU_API mizu_status mizu_pool_submit_flags(mizu_pool *, void *task_obj,
                                         uint16_t flags, mizu_task *out,
                                         double timeout_ms);

// Map morsel protocol (morsel.c) -------------------------------------------------

/* A binding's parallel map rides one fresh region per map call: a 128-byte
   header, the language-specific descriptor stream, an optional bare-bytes x
   section, the morsel state, and an optional template output area. Map
   regions are private to a binding install (workers spawn from the same
   package), so the layout is unified here and each binding keeps only its
   magic tag. The protocol:
   - one CLAIM word per runner *ordinal*, packing (generation << 2) | state,
     so the lane claim and the generation fence are one atomic (a
     check-then-CAS would leave a TOCTOU window against reset's re-arm).
   - the shared cursor is a relaxed ticket dispenser; ordering rides the
     task claim/publish chain. An overshoot of up to k morsels is harmless —
     a runner stops at its first exhausted issue.
   - completion is never recorded: runners publish their batch histories
     through ordinary results, and the lost set on worker death is
     arithmetic over them (mizu_morsel_lost).
   The descriptor codec, the x-section element I/O, the batch loop, and the
   result gather stay binding-side. */

#define MIZU_MORSEL_CANCEL_OFF ((uint64_t) 0)
#define MIZU_MORSEL_GEN_OFF    ((uint64_t) 4)
#define MIZU_MORSEL_CURSOR_OFF ((uint64_t) 64)
#define MIZU_MORSEL_CLAIM_OFF  ((uint64_t) 128)
/* Generation comparisons mask to the CLAIM word's 30 bits (wrap takes 2^30
   resets of one region: harmless). */
#define MIZU_MORSEL_GEN_MASK   ((uint32_t) 0x3FFFFFFF)

enum { MIZU_MORSEL_IDLE = 0, MIZU_MORSEL_RUNNING, MIZU_MORSEL_ABANDONED };
enum { MIZU_MORSEL_X_DESC = 0, MIZU_MORSEL_X_RAW };

/* Batch sizing policy (mizu_morsel_next): k targets a batch duration,
   growing at most 2x per step and shrinking immediately on overshoot,
   clamped to the cap — which bounds lost-set coarseness and the ramp worst
   case. Frozen by mizu's 2026-08-04 gate sweep (M4 Pro, W = 4). */
#define MIZU_MORSEL_T_TARGET  200e-6
#define MIZU_MORSEL_BATCH_CAP 64

/* The one map-region magic: "MIZM" read as hex, the core's magic packing
   (MIZU_MAGIC, the pool magic, MIZH/MIZS/MIZL). The morsel module stamps
   and checks it itself — the descriptor's codec identity rides the
   descriptor stream's own first byte, so the magic carried nothing else,
   and no path can hand a runner another binding's region. */
#define MIZU_MORSEL_MAGIC 0x4D495A4Du

/* The 128-byte region header. Not pool wire format — it rides its own
   region, keyed by the same ABI version — but the same rules apply: the
   struct is the layout, 64-byte-aligned sections follow it. */
typedef struct mizu_morsel_hdr_s {
  uint32_t magic;          /* MIZU_MORSEL_MAGIC */
  uint32_t version;        /* MIZU_ABI_VERSION */
  uint32_t flags;          /* reserved, 0 */
  uint32_t x_kind;         /* MIZU_MORSEL_X_* */
  uint32_t x_type;         /* raw x section wire type (MIZU_TYPE_*) */
  uint32_t out_type;       /* template element wire type; 0 = no output */
  uint32_t out_elt;        /* template element size, bytes */
  uint32_t claim_n;        /* CLAIM word count (runner ordinal bound) */
  uint64_t n;              /* map elements */
  uint64_t desc_off, desc_len;
  uint64_t x_off, x_len;
  uint64_t out_off;
  uint64_t out_m;          /* template length: values per element */
  uint64_t morsel_size;    /* elements per morsel */
  uint64_t n_morsels;      /* ceiling(n / morsel_size) */
  uint64_t state_off;      /* morsel state section offset */
  uint8_t  pad[16];
} mizu_morsel_hdr;
MIZU_STATIC_ASSERT(sizeof(mizu_morsel_hdr) == 128,
                  "mizu_morsel_hdr is the wire format");

/* Fill *h with the section layout for a map of n elements (morsel_size the
   geometry input, desc_len the descriptor stream size, x_type/x_len the
   optional raw x section — x_len must be exactly n elements — out_type/out_m
   the optional template output area, claim_n the CLAIM word count). Sections
   land 64-aligned in order: descriptor, x, morsel state, output. Returns the
   region size, 0 on invalid geometry or overflow (the binding raises its own
   error). The caller memcpys the header into the fresh region. */
MIZU_API uint64_t mizu_morsel_layout(mizu_morsel_hdr *h,
                                   uint64_t n, uint64_t morsel_size,
                                   uint64_t desc_len, uint32_t x_type,
                                   uint64_t x_len, uint32_t out_type,
                                   uint64_t out_m, uint32_t claim_n);
/* Validate a region's header against the mapping size, including section
   placement. Returns NULL and fills *out (when non-NULL) on success, else
   a static message. Runs once per mapping. */
MIZU_API const char *mizu_morsel_hdr_check(const void *base, size_t size,
                                         mizu_morsel_hdr *out);

/* Batch-sizing state: process-private, never wire state, reset at each
   run's first-call CLAIM CAS. The binding embeds one per map context; a
   doorbell help that claims a queued runner of the same map aliases it (the
   cost is a mis-sized batch or a re-ramp on resume — harmless). */
typedef struct mizu_morsel_sizer_s {
  int32_t  run_r;          /* ordinal whose ramp this is (-1 = none) */
  uint32_t run_gen;
  uint64_t k;              /* current batch size, morsels */
  uint64_t k_last;         /* morsels issued last transition */
  double   t_last;         /* mizu_now() at the last issue */
  double   cost;           /* est. seconds per morsel (0 = unknown) */
  int      skip;           /* last interval contained a help: no update */
} mizu_morsel_sizer;
MIZU_API void mizu_morsel_sizer_init(mizu_morsel_sizer *);

/* One whole batch transition — generation-fenced lane claim, cancel and
   pool-signal checks, sized cursor issue — over the mapped region. Returns
   1 and fills m (the 0-based first morsel), k (morsel count after the final
   partial grant) and help (the doorbell flag); 0 means stop: the lane was
   lost to the trim or a reset re-armed it, the cancel word fired, the pool
   is stopping, the owner died, or the cursor is exhausted. sig (may be
   NULL) is the worker-local pool-signal trio; pin_k != 0 fixes k (a test
   entry, bypassing the sizing policy); now is the caller's mizu_now() read.
   Atomics only, no park: safe to call under a global lock. */
MIZU_API int mizu_morsel_next(void *base, const mizu_morsel_hdr *h,
                            mizu_morsel_sizer *sz, uint32_t r, uint32_t gen,
                            const mizu_pool_sig *sig, uint64_t pin_k,
                            double now, uint64_t *m_out, uint64_t *k_out,
                            int *help_out);

/* Prepared-run re-arm, O(1) in n (no per-morsel state exists to clear):
   bump the generation, stamp (new_gen << 2) | IDLE over the CLAIM array,
   zero the cursor, clear the cancel word. The stamped generation is the
   fence against a stale trimmed runner from the prior run: its first-call
   CAS expects the old generation and fails against the re-armed word
   however the reset interleaves. Returns the new generation — the value the
   next run's runner payloads must carry. */
MIZU_API uint32_t mizu_morsel_reset(void *base, const mizu_morsel_hdr *h);

/* The exhausted-runner trim's CAS, folding its own trigger: a no-op unless
   the cursor is exhausted or the cancel word is set. Returns the
   morsel-state verdict: MIZU_MORSEL_ABANDONED (won, or already trimmed),
   MIZU_MORSEL_RUNNING (executing or published — collect it),
   MIZU_MORSEL_IDLE (trigger unarmed: defer rather than park). */
MIZU_API int mizu_morsel_abandon(void *base, const mizu_morsel_hdr *h,
                               uint32_t r, uint32_t gen);

/* The cancel word: set by the submitter on timeout / interrupt / death, and
   by an erroring runner itself before its ERR publish — the fail-fast
   store that stops every peer within ~a batch. Idempotent. */
MIZU_API void mizu_morsel_cancel_set(void *base, const mizu_morsel_hdr *h);
MIZU_API int mizu_morsel_cancel_get(const void *base, const mizu_morsel_hdr *h);
/* Single reads of the mutable words: the generation (masked), the cursor
   clamped to n_morsels, and one CLAIM word (the protocol tests' view). */
MIZU_API uint32_t mizu_morsel_generation(const void *base,
                                       const mizu_morsel_hdr *h);
MIZU_API uint64_t mizu_morsel_cursor(const void *base,
                                   const mizu_morsel_hdr *h);
MIZU_API uint32_t mizu_morsel_claim(const void *base, const mizu_morsel_hdr *h,
                                  uint32_t r);

/* A batch's element range, 0-based half-open: [m * morsel_size, min((m + k)
   * morsel_size, n)). Bindings with 1-based inclusive ranges add 1 to lo. */
MIZU_API void mizu_morsel_span_of(const mizu_morsel_hdr *h, uint64_t m,
                                uint64_t k, uint64_t *lo, uint64_t *hi);

/* Worker-death lost set: issued = [0, bound), lost = issued minus the union
   of the collected batch histories — a batch in no history was issued but
   never completed (its claimant died, or the map fn errored mid-batch); a
   dead runner's whole history lands here too — it publishes only at
   exhaustion. spans (caller-assembled from the histories, element space,
   lo <= hi) is sorted in place; the gaps (at most n + 1) land in out as
   0-based half-open ranges; returns the gap count. */
typedef struct mizu_morsel_span_s { uint64_t lo, hi; } mizu_morsel_span;
MIZU_API size_t mizu_morsel_lost(mizu_morsel_span *spans, size_t n,
                               uint64_t bound, mizu_morsel_span *out);

// The result sink and the unwind path (pool.c) ----------------------------------

/* The result-sink struct: the core fills it from a claimed entry and
   hands it to exec_fn; the binding passes it back to the publish verbs.
   payload/inline_max expose the slot's frame buffer so the binding
   frames its ERR envelope inline. */
struct mizu_result_sink_s {
  mizu_pool     *p;
  mizu_rs_hdr   *rs;
  unsigned char *payload;   /* rs + sizeof(mizu_rs_hdr): the frame buffer */
  uint32_t      rs_index;
  uint32_t      inline_max; /* the slot's payload capacity */
  uint16_t      sub_slot;   /* the task's submitter (zc keying, probes) */
  uint64_t      seq;        /* rs->sequence at claim */
  uint64_t      task_id;
};

/* The eval-in-flight marker a binding sets around its catching = 0 task
   eval only (R: around Rf_eval, never the decode), so its unwind path
   can tell a task error from infrastructure failure. The core heals it
   at worker step/run entry. */
MIZU_API void mizu_pool_eval_mark(mizu_pool *, int in_flight);
/* The unwind path for a binding whose catching = 0 exec abandons (R's
   longjmp): when the eval marker says a task eval was in flight, clears
   it and mints that task's sink (from the identity the core saved at
   execute) for the err publish. Returns 1 then; 0 when the abandonment
   came from outside any task eval — infrastructure failure, the worker
   goes down. */
MIZU_API int mizu_pool_unwind_sink(mizu_pool *, mizu_result_sink *out);

// Zero-copy refcount words ---------------------------------------------------------

/* The zc refcount / flags words of a MIZU* region header (bytes [24-31]
   of the reserved band — the wire-format offsets MIZU_ZC_REFCOUNT_OFF /
   MIZU_ZC_FLAGS_OFF in mizu.h). Shared by the core release machinery and
   the bindings' view wrap/resolve paths. Dual form (see the banner):
   the exported symbols in src/ext.c are these same bodies. */
#ifdef MIZU_EXT_NO_INLINES
MIZU_API MIZU_ATOMIC(uint32_t) *mizu_zc_rc(void *base);
MIZU_API MIZU_ATOMIC(uint32_t) *mizu_zc_flags_(void *base);
#else
MIZU_EXT_INLINE MIZU_ATOMIC(uint32_t) *mizu_zc_rc(void *base) {
  return (MIZU_ATOMIC(uint32_t) *) ((unsigned char *) base +
                                   MIZU_ZC_REFCOUNT_OFF);
}
MIZU_EXT_INLINE MIZU_ATOMIC(uint32_t) *mizu_zc_flags_(void *base) {
  return (MIZU_ATOMIC(uint32_t) *) ((unsigned char *) base +
                                   MIZU_ZC_FLAGS_OFF);
}
#endif

// Wire-format helpers --------------------------------------------------------------

/* The small pure helpers over mizu.h's wire-format conventions, dual form
   like the zc accessors above (see the banner): the timeout unit
   conversion, the NA_real_ store (for a binding filling converted output
   without R headers), the aux packing of the pool RAWSPILL and SHM_VEC
   kinds and its decode pair, the flat MIZH header write/check, the MIZS /
   MIZL layout checks, the validity-section setter, and the NA sentinel
   <-> bitmap primitives (one NA-test implementation behind both
   bindings' validity paths, so two bindings' bitmaps cannot drift apart
   from the sentinels they describe). The layout layouts themselves are
   mizu.h's MIZH / MIZS / MIZL documentation. */

/* The MIZS string block's four section offsets from a string count
   (mizu.h documents the block). data doubles as the size of everything
   before the string bytes. */
typedef struct mizu_mizs_geom_s {
  int64_t validity;
  int64_t offsets;
  int64_t encoding;
  int64_t data;
} mizu_mizs_geom;

/* One MIZL directory entry plus its validity-table pair (mizu.h documents
   the 32-byte entry): mizu_mizl_elem's out-param. sexptype is the wire
   value — MIZU_MIZL_S4 is the S4 bit, the remainder a listed tag. */
typedef struct mizu_mizl_entry_s {
  int64_t data_offset;   /* 64-byte aligned */
  int64_t data_size;
  int32_t sexptype;
  int32_t attrs_size;
  int64_t length;
  int64_t valid[2];      /* {0, 0} absent, {0, -1} known-NA-free, else
                            {bitmap offset, null count} */
} mizu_mizl_entry;

/* The string block's section offsets for n strings (each section 64-byte
   aligned from the block start). Dual form, like the helpers below. */
#ifdef MIZU_EXT_NO_INLINES
MIZU_API mizu_mizs_geom mizu_mizs_geometry(int64_t n);
#else
MIZU_EXT_INLINE mizu_mizs_geom mizu_mizs_geometry(int64_t n) {
  mizu_mizs_geom g;
  g.validity = 0;
  g.offsets  = (int64_t) MIZU_ALIGN64((uint64_t) (n + 7) / 8);
  g.encoding = g.offsets + (int64_t) MIZU_ALIGN64(8 * ((uint64_t) n + 1));
  g.data     = g.encoding + (int64_t) MIZU_ALIGN64((uint64_t) n);
  return g;
}
#endif

/* The [32-35] format flags word admits only the assigned S4 bit. */
MIZU_EXT_INLINE int mizu_ext_flags_known(const void *base) {
  uint32_t flags;
  memcpy(&flags, (const unsigned char *) base + MIZU_HDR_FLAGS_OFF, 4);
  return (flags & ~MIZU_HDR_FLAG_S4) == 0;
}

/* The three-state validity pair of an MIZH header or one MIZL leaf-table
   entry: {0, 0} absent, {0, -1} known-NA-free, or a 64-byte-aligned
   offset whose ceil(n / 8)-byte bitmap fits the region, 0 <= count <= n. */
MIZU_EXT_INLINE int mizu_ext_valid_ok(int64_t off, int64_t count,
                                     uint64_t n, size_t size) {
  if (off == 0) return count == 0 || count == -1;
  if (count < 0 || (uint64_t) count > n || (off & 63) != 0) return 0;
  const uint64_t bytes = (n + 7) / 8;
  return (uint64_t) off <= (uint64_t) size &&
         bytes <= (uint64_t) size - (uint64_t) off;
}

/* A directory entry's sexptype: MIZU_MIZL_S4 masked off, the remainder a
   listed tag — 0 (a serialized leaf), the atomic tags, STR, VEC, INT64.
   The reserved remote leaf (33) rejects with anything else unlisted. */
MIZU_EXT_INLINE int mizu_ext_mizl_tag_ok(int32_t sexptype) {
  switch (sexptype & ~(int32_t) MIZU_MIZL_S4) {
  case 0:
  case MIZU_TYPE_LGL:
  case MIZU_TYPE_INT:
  case MIZU_TYPE_REAL:
  case MIZU_TYPE_CPLX:
  case MIZU_TYPE_STR:
  case MIZU_TYPE_VEC:
  case MIZU_TYPE_RAW:
  case MIZU_TYPE_INT64:
    return 1;
  default:
    return 0;
  }
}

/* One per-element NA test per NA-capable wire type (anything else is
   never null): INT32_MIN for LGL and INT, INT64_MIN, the NA_real_
   payload discriminated from other NaNs, the CPLX pair either part. */
MIZU_EXT_INLINE int mizu_ext_na_at(int type, const void *src, uint64_t i) {
  const unsigned char *p = (const unsigned char *) src;
  uint64_t bits, re, im;
  int32_t v32;
  int64_t v64;
  switch (type) {
  case MIZU_TYPE_LGL:
  case MIZU_TYPE_INT:
    memcpy(&v32, p + 4 * i, 4);
    return v32 == MIZU_NA_INT32;
  case MIZU_TYPE_REAL:
    memcpy(&bits, p + 8 * i, 8);
    return bits == MIZU_NA_REAL_BITS;
  case MIZU_TYPE_CPLX:
    memcpy(&re, p + 16 * i, 8);
    memcpy(&im, p + 16 * i + 8, 8);
    return re == MIZU_NA_REAL_BITS || im == MIZU_NA_REAL_BITS;
  case MIZU_TYPE_INT64:
    memcpy(&v64, p + 8 * i, 8);
    return v64 == MIZU_NA_INT64;
  default:
    return 0;
  }
}

/* Write the wire type's missing sentinel over element i (the CPLX pair
   both parts). */
MIZU_EXT_INLINE void mizu_ext_na_store(int type, void *dst, uint64_t i) {
  unsigned char *p = (unsigned char *) dst;
  const int32_t na32 = MIZU_NA_INT32;
  const int64_t na64 = MIZU_NA_INT64;
  const uint64_t nabits = MIZU_NA_REAL_BITS;
  switch (type) {
  case MIZU_TYPE_LGL:
  case MIZU_TYPE_INT:
    memcpy(p + 4 * i, &na32, 4);
    break;
  case MIZU_TYPE_REAL:
    memcpy(p + 8 * i, &nabits, 8);
    break;
  case MIZU_TYPE_CPLX:
    memcpy(p + 16 * i, &nabits, 8);
    memcpy(p + 16 * i + 8, &nabits, 8);
    break;
  case MIZU_TYPE_INT64:
    memcpy(p + 8 * i, &na64, 8);
    break;
  default:
    break;
  }
}

/* One MIZL directory entry's checks, shared by mizu_mizl_check's pass and
   mizu_mizl_elem: alignment and extent, the attrs tail, the listed tag,
   and the length against the leaf kind — the string block's fixed
   sections for STR, the element extent for an atomic leaf. Fills *e
   except the valid pair. */
MIZU_EXT_INLINE int mizu_ext_mizl_ent(const void *base, size_t size,
                                     int64_t i, mizu_mizl_entry *e) {
  const unsigned char *dir = (const unsigned char *) base +
    MIZU_HEADER_SIZE + 32 * (size_t) i;
  memcpy(&e->data_offset, dir, 8);
  memcpy(&e->data_size, dir + 8, 8);
  memcpy(&e->sexptype, dir + 16, 4);
  memcpy(&e->attrs_size, dir + 20, 4);
  memcpy(&e->length, dir + 24, 8);
  if (e->data_offset < 0 || (e->data_offset & 63) != 0 ||
      e->data_size < 0 ||
      (uint64_t) e->data_offset > (uint64_t) size ||
      (uint64_t) e->data_size >
        (uint64_t) size - (uint64_t) e->data_offset ||
      e->attrs_size < 0 || (int64_t) e->attrs_size > e->data_size)
    return -1;
  if (!mizu_ext_mizl_tag_ok(e->sexptype)) return -1;
  const int32_t tag = e->sexptype & ~(int32_t) MIZU_MIZL_S4;
  const int64_t body = e->data_size - (int64_t) e->attrs_size;
  const size_t elt = mizu_type_elt_size(tag);
  if (elt != 0) {
    if (e->length < 0 || e->length > body / (int64_t) elt) return -1;
  } else if (tag == MIZU_TYPE_STR) {
    if (e->length < 0 || e->length > ((int64_t) 1 << 50) ||
        mizu_mizs_geometry(e->length).data > body)
      return -1;
  } else if (tag == MIZU_TYPE_VEC) {
    if (e->length < 0) return -1;
  }
  return 0;
}

/* The bodies behind the larger dual-form functions below: both the
   header inline and the exported symbol in src/ext.c are one-line
   delegations to these, so the two forms cannot drift apart. */

MIZU_EXT_INLINE int mizu_ext_mizh_check_impl(const void *base, size_t size,
                                            int *wire_type, int64_t *n_elems,
                                            int64_t valid[2]) {
  if (size < MIZU_HEADER_SIZE) return -1;
  uint32_t magic;
  int32_t t32;
  int64_t len, attrs, voff, vcount;
  memcpy(&magic, base, 4);
  if (magic != MIZU_MAGIC_VEC) return -1;
  memcpy(&t32, (const unsigned char *) base + 4, 4);
  memcpy(&len, (const unsigned char *) base + 8, 8);
  memcpy(&attrs, (const unsigned char *) base + 16, 8);
  memcpy(&voff, (const unsigned char *) base + MIZU_HDR_VALID_OFF, 8);
  memcpy(&vcount, (const unsigned char *) base + MIZU_HDR_VALID_COUNT, 8);
  const size_t elt = mizu_type_elt_size(t32);
  if (elt == 0 || len < 0 || attrs < 0 ||
      len > ((int64_t) size - (int64_t) MIZU_HEADER_SIZE) / (int64_t) elt ||
      attrs > (int64_t) size - (int64_t) MIZU_HEADER_SIZE -
              len * (int64_t) elt)
    return -1;
  if (!mizu_ext_flags_known(base) ||
      !mizu_ext_valid_ok(voff, vcount, (uint64_t) len, size))
    return -1;
  *wire_type = t32;
  *n_elems = len;
  valid[0] = voff;
  valid[1] = vcount;
  return 0;
}

MIZU_EXT_INLINE int mizu_ext_mizs_check_impl(const void *base, size_t size,
                                            int64_t *n, int64_t *str_size,
                                            int64_t *attrs_size) {
  if (size < MIZU_HEADER_SIZE) return -1;
  uint32_t magic;
  int32_t attrs;
  int64_t count, block;
  memcpy(&magic, base, 4);
  if (magic != MIZU_MAGIC_STR) return -1;
  memcpy(&attrs, (const unsigned char *) base + 4, 4);
  memcpy(&count, (const unsigned char *) base + 8, 8);
  memcpy(&block, (const unsigned char *) base + 16, 8);
  if (attrs < 0 || count < 0 || count > ((int64_t) 1 << 50) || block < 0)
    return -1;
  if (!mizu_ext_flags_known(base)) return -1;
  const int64_t fixed = mizu_mizs_geometry(count).data;
  if (fixed > block ||
      block > (int64_t) size - (int64_t) MIZU_HEADER_SIZE ||
      (int64_t) attrs >
        (int64_t) size - (int64_t) MIZU_HEADER_SIZE - block)
    return -1;
  *n = count;
  *str_size = block;
  *attrs_size = attrs;
  return 0;
}

MIZU_EXT_INLINE int mizu_ext_mizl_check_impl(const void *base, size_t size,
                                            int64_t *n, int64_t *attrs_off,
                                            int64_t *attrs_size,
                                            int64_t valid[2]) {
  if (size < MIZU_HEADER_SIZE) return -1;
  uint32_t magic;
  int32_t n32;
  int64_t aoff, asz, voff, vcount;
  memcpy(&magic, base, 4);
  if (magic != MIZU_MAGIC_LIST) return -1;
  memcpy(&n32, (const unsigned char *) base + 4, 4);
  memcpy(&aoff, (const unsigned char *) base + 8, 8);
  memcpy(&asz, (const unsigned char *) base + 16, 8);
  memcpy(&voff, (const unsigned char *) base + MIZU_HDR_VALID_OFF, 8);
  memcpy(&vcount, (const unsigned char *) base + MIZU_HDR_VALID_COUNT, 8);
  if (n32 < 0 ||
      (uint64_t) (uint32_t) n32 >
        ((uint64_t) size - (uint64_t) MIZU_HEADER_SIZE) / 32)
    return -1;
  if (aoff < 0 || asz < 0 ||
      (uint64_t) aoff > (uint64_t) size ||
      (uint64_t) asz > (uint64_t) size - (uint64_t) aoff)
    return -1;
  if (!mizu_ext_flags_known(base)) return -1;
  uint64_t sum = 0;
  for (int64_t i = 0; i < (int64_t) n32; i++) {
    mizu_mizl_entry e;
    if (mizu_ext_mizl_ent(base, size, i, &e) != 0) return -1;
    const int32_t tag = e.sexptype & ~(int32_t) MIZU_MIZL_S4;
    if (mizu_type_elt_size(tag) != 0) sum += (uint64_t) e.length;
  }
  if (voff == 0) {
    if (vcount != 0 && vcount != -1) return -1;
  } else {
    const uint64_t bytes = (uint64_t) (uint32_t) n32 * 16;
    if (vcount < 0 || (uint64_t) vcount > sum || (voff & 63) != 0 ||
        (uint64_t) voff > (uint64_t) size ||
        bytes > (uint64_t) size - (uint64_t) voff)
      return -1;
  }
  *n = n32;
  *attrs_off = aoff;
  *attrs_size = asz;
  valid[0] = voff;
  valid[1] = vcount;
  return 0;
}

MIZU_EXT_INLINE int mizu_ext_mizl_elem_impl(const void *base, size_t size,
                                           int64_t i,
                                           mizu_mizl_entry *elem) {
  if (size < MIZU_HEADER_SIZE) return -1;
  uint32_t magic;
  int32_t n32;
  int64_t voff, vcount;
  memcpy(&magic, base, 4);
  if (magic != MIZU_MAGIC_LIST) return -1;
  memcpy(&n32, (const unsigned char *) base + 4, 4);
  if (n32 < 0 || i < 0 || i >= (int64_t) n32 ||
      (uint64_t) (uint32_t) n32 >
        ((uint64_t) size - (uint64_t) MIZU_HEADER_SIZE) / 32)
    return -1;
  if (!mizu_ext_flags_known(base)) return -1;
  if (mizu_ext_mizl_ent(base, size, i, elem) != 0) return -1;
  memcpy(&voff, (const unsigned char *) base + MIZU_HDR_VALID_OFF, 8);
  memcpy(&vcount, (const unsigned char *) base + MIZU_HDR_VALID_COUNT, 8);
  if (voff == 0) {
    if (vcount != 0 && vcount != -1) return -1;
    elem->valid[0] = 0;
    elem->valid[1] = vcount;
    return 0;
  }
  if ((voff & 63) != 0) return -1;
  const uint64_t bytes = (uint64_t) (uint32_t) n32 * 16;
  if ((uint64_t) voff > (uint64_t) size ||
      bytes > (uint64_t) size - (uint64_t) voff)
    return -1;
  const unsigned char *tab = (const unsigned char *) base + voff;
  int64_t loff, lcount;
  memcpy(&loff, tab + 16 * (size_t) i, 8);
  memcpy(&lcount, tab + 16 * (size_t) i + 8, 8);
  if (!mizu_ext_valid_ok(loff, lcount, (uint64_t) elem->length, size))
    return -1;
  elem->valid[0] = loff;
  elem->valid[1] = lcount;
  return 0;
}

MIZU_EXT_INLINE uint64_t mizu_ext_na_build_impl(int type, uint8_t *bitmap,
                                               const void *src, uint64_t n,
                                               uint64_t bit_off) {
  uint64_t nulls = 0;
  for (uint64_t i = 0; i < n; i++) {
    const uint64_t bit = bit_off + i;
    if (mizu_ext_na_at(type, src, i)) {
      bitmap[bit >> 3] &= (uint8_t) ~(1u << (bit & 7));
      nulls++;
    } else {
      bitmap[bit >> 3] |= (uint8_t) (1u << (bit & 7));
    }
  }
  return nulls;
}

MIZU_EXT_INLINE uint64_t mizu_ext_na_apply_impl(int type, void *dst,
                                               const void *src,
                                               const uint8_t *bitmap,
                                               uint64_t n) {
  const size_t elt = mizu_type_elt_size(type);
  if (elt == 0) return 0;
  if (dst != src) memcpy(dst, src, (size_t) n * elt);
  uint64_t nulls = 0;
  for (uint64_t i = 0; i < n; i++) {
    if (((bitmap[i >> 3] >> (i & 7)) & 1) == 0) {
      mizu_ext_na_store(type, dst, i);
      nulls++;
    }
  }
  return nulls;
}

#ifdef MIZU_EXT_NO_INLINES
MIZU_API double mizu_timeout_ms(double seconds);
MIZU_API void mizu_store_na_real(void *dst);
MIZU_API uint64_t mizu_aux_rawspill_pool(int type, uint32_t name_len);
MIZU_API uint64_t mizu_aux_shm_vec(int type, uint64_t total);
MIZU_API int mizu_aux_type(uint64_t aux);
MIZU_API uint64_t mizu_aux_hi(uint64_t aux);
MIZU_API void mizu_mizh_write(void *base, int wire_type, int64_t n_elems);
MIZU_API int mizu_mizh_check(const void *base, size_t size,
                           int *wire_type, int64_t *n_elems, int64_t valid[2]);
MIZU_API void mizu_mizh_validity_set(void *base, int64_t off,
                                   int64_t count);
MIZU_API int mizu_mizs_check(const void *base, size_t size, int64_t *n,
                           int64_t *str_size, int64_t *attrs_size);
MIZU_API int mizu_mizl_check(const void *base, size_t size, int64_t *n,
                           int64_t *attrs_off, int64_t *attrs_size,
                           int64_t valid[2]);
MIZU_API int mizu_mizl_elem(const void *base, size_t size, int64_t i,
                           mizu_mizl_entry *elem);
MIZU_API uint64_t mizu_na_build(int type, uint8_t *bitmap, const void *src,
                              uint64_t n, uint64_t bit_off);
MIZU_API uint64_t mizu_na_apply(int type, void *dst, const void *src,
                              const uint8_t *bitmap, uint64_t n);
#else
/* seconds (a binding's convention; <= 0 polls, non-finite waits
   indefinitely) to the core's timeout_ms (0 polls, < 0 indefinite). */
MIZU_EXT_INLINE double mizu_timeout_ms(double seconds) {
  if (!isfinite(seconds)) return -1;
  return seconds <= 0 ? 0 : seconds * 1000;
}
MIZU_EXT_INLINE void mizu_store_na_real(void *dst) {
  const uint64_t bits = MIZU_NA_REAL_BITS;
  memcpy(dst, &bits, 8);
}
/* aux = wire type tag | region name length << 8. */
MIZU_EXT_INLINE uint64_t mizu_aux_rawspill_pool(int type, uint32_t name_len) {
  return (uint64_t) (uint32_t) type | ((uint64_t) name_len << 8);
}
/* aux = layout type tag | exact used bytes << 8. */
MIZU_EXT_INLINE uint64_t mizu_aux_shm_vec(int type, uint64_t total) {
  return (uint64_t) (uint32_t) type | (total << 8);
}
/* The decode half of the aux split (low byte the type tag, the kind's
   field above it): the tag, and the packed field (the pool RAWSPILL
   region name length; the SHM_VEC exact used bytes). */
MIZU_EXT_INLINE int mizu_aux_type(uint64_t aux) {
  return (int) (aux & 0xff);
}
MIZU_EXT_INLINE uint64_t mizu_aux_hi(uint64_t aux) {
  return aux >> 8;
}
/* The flat MIZH header: magic, type, element count, zero attrs, and the
   reserved band [24, 64) zeroed. Write BEFORE mizu_stage_retain_zc: the
   zeroing covers the zc refcount word (a recycled region carries a stale
   count), and the retain stores the producer loan after. */
MIZU_EXT_INLINE void mizu_mizh_write(void *base, int wire_type,
                                   int64_t n_elems) {
  const uint32_t magic = MIZU_MAGIC_VEC;
  const int32_t t32 = wire_type;
  const int64_t zero64 = 0;
  memcpy(base, &magic, 4);
  memcpy((unsigned char *) base + 4, &t32, 4);
  memcpy((unsigned char *) base + 8, &n_elems, 8);
  memcpy((unsigned char *) base + 16, &zero64, 8);
  memset((unsigned char *) base + 24, 0, MIZU_HEADER_SIZE - 24);
}
/* Validate the MIZH header at base: region size, magic, a known atomic
   wire type, the element/attribute extents against the region size, the
   format flags word, and the validity section (handed back through
   valid) — 0 on success, -1 on any rejection. Attribute policy (whether
   the word at [16, 24) may be nonzero, and what it means) stays
   binding-side. */
MIZU_EXT_INLINE int mizu_mizh_check(const void *base, size_t size,
                                  int *wire_type, int64_t *n_elems,
                                  int64_t valid[2]) {
  return mizu_ext_mizh_check_impl(base, size, wire_type, n_elems, valid);
}
/* Stamp the validity pair of an MIZH or MIZL header (the one write site):
   {0, 0} absent, {0, -1} known-NA-free, or the section offset and null
   count. */
MIZU_EXT_INLINE void mizu_mizh_validity_set(void *base, int64_t off,
                                          int64_t count) {
  memcpy((unsigned char *) base + MIZU_HDR_VALID_OFF, &off, 8);
  memcpy((unsigned char *) base + MIZU_HDR_VALID_COUNT, &count, 8);
}
/* Validate the MIZS header at base against the region: magic, the format
   flags word, and the string block's fixed sections plus the two end
   offsets (the block's and the attrs blob's) — 0 on success, -1 on any
   rejection. The block's own validity bitmap serves where MIZH / MIZL
   carry the header section, so there is no valid out-param. */
MIZU_EXT_INLINE int mizu_mizs_check(const void *base, size_t size,
                                  int64_t *n, int64_t *str_size,
                                  int64_t *attrs_size) {
  return mizu_ext_mizs_check_impl(base, size, n, str_size, attrs_size);
}
/* Validate the MIZL header at base and its directory's extent against
   the region: magic, the format flags word, every entry (mizu_ext_mizl_ent),
   and the header's validity pair — whose count totals the nulls across
   the leaves, so the directory entry count does not bound it; the sum of
   the atomic leaves' lengths does. 0 on success, -1 on any rejection. */
MIZU_EXT_INLINE int mizu_mizl_check(const void *base, size_t size,
                                  int64_t *n, int64_t *attrs_off,
                                  int64_t *attrs_size, int64_t valid[2]) {
  return mizu_ext_mizl_check_impl(base, size, n, attrs_off, attrs_size,
                                 valid);
}
/* One bounds-checked MIZL directory entry (index i), with its
   validity-table pair: a {0, 0}/{0, -1} header state covers every leaf;
   otherwise the table's entry i is validated by the MIZH rule against
   the leaf's length. 0 on success, -1 on any rejection. */
MIZU_EXT_INLINE int mizu_mizl_elem(const void *base, size_t size, int64_t i,
                                 mizu_mizl_entry *elem) {
  return mizu_ext_mizl_elem_impl(base, size, i, elem);
}
/* The sentinels-to-bitmap scan: bit (bit_off + i) of bitmap records
   element i of src, 1 = present (the MIZS string block's convention);
   a type with no missing sentinel is all-present. Returns the null
   count. */
MIZU_EXT_INLINE uint64_t mizu_na_build(int type, uint8_t *bitmap,
                                     const void *src, uint64_t n,
                                     uint64_t bit_off) {
  return mizu_ext_na_build_impl(type, bitmap, src, n, bit_off);
}
/* The bitmap-to-sentinels copy: n elements land at dst, the clear bits'
   elements written as the wire type's missing sentinel. Returns the
   null count. */
MIZU_EXT_INLINE uint64_t mizu_na_apply(int type, void *dst, const void *src,
                                     const uint8_t *bitmap, uint64_t n) {
  return mizu_ext_na_apply_impl(type, dst, src, bitmap, n);
}
#endif

// The interchange stream ('I') ---------------------------------------------------

/* The byte-level half of DESIGN.md's Interchange codec section: a
   validating pull cursor (mizu_ix_open / mizu_ix_next / mizu_ix_end, in
   src/interop.c) and the dual-form emit helpers below, shared by every
   binding so the wire grammar has exactly one implementation. The cursor
   owns bounds, the depth cap, UTF-8 validity, the container arity
   accounting (a task is one element of kind-determined arity), and the
   informative unknown-tag / unknown-version / unknown-kind declines,
   recording them in the thread-local error slot
   (mizu_last_error_message). A binding keeps only a builder, allocating a
   native object per item, and a value walk emitting through the put
   helpers; the golden corpus (tests/interop/) certifies the grammar once
   here. */

#define MIZU_IX_VERSION 0x01u
#define MIZU_IX_DEPTH_MAX 64

/* The wire tags (DESIGN.md's tag table). */
enum {
  MIZU_IX_TAG_NIL   = 0x00,
  MIZU_IX_TAG_LGL1  = 0x01,
  MIZU_IX_TAG_INT   = 0x02,
  MIZU_IX_TAG_REAL  = 0x03,
  MIZU_IX_TAG_STR   = 0x04,
  MIZU_IX_TAG_BYTES = 0x05,
  MIZU_IX_TAG_LGLV  = 0x06,
  MIZU_IX_TAG_INTV  = 0x07,
  MIZU_IX_TAG_REALV = 0x08,
  MIZU_IX_TAG_CPLXV = 0x09,
  MIZU_IX_TAG_RAWV  = 0x0a,
  MIZU_IX_TAG_STRV  = 0x0b,
  MIZU_IX_TAG_LIST  = 0x0c,
  MIZU_IX_TAG_DICT  = 0x0d,
  MIZU_IX_TAG_I64V  = 0x0e,
  MIZU_IX_TAG_ATTR  = 0x0f,
  MIZU_IX_TAG_CPLX  = 0x10,
  MIZU_IX_TAG_ERR   = 0x11,
  MIZU_IX_TAG_TASK  = 0x12
};

/* What mizu_ix_next yields. A scalar carries its value; STR1 (the 0x04
   scalar) and STR (a bare string: an strv element or a dict key — key is
   1 for the latter) carry a UTF-8-validated span, na set on the -1 form
   (keys are never na); BYTES and VEC carry a (ptr, count) span already
   bounds-checked; STRV / LIST / DICT / ATTR are counted begins whose
   elements arrive as following items (a dict's keys as STR items); ERR
   carries its fields (top level only); TASK is a header item whose
   kind-determined arity (count) of fields arrive as ordinary items. */
enum {
  MIZU_IX_NIL = 0,
  MIZU_IX_LGL,          /* u64[0]: 0, 1, or 2 (NA) */
  MIZU_IX_INT,          /* u64[0]: the i64 */
  MIZU_IX_REAL,         /* u64[0]: the f64 bits */
  MIZU_IX_CPLX,         /* u64[0..1]: the re / im f64 bits */
  MIZU_IX_STR1,         /* the 0x04 string scalar */
  MIZU_IX_STR,          /* a bare string (strv element, or dict key: key=1) */
  MIZU_IX_BYTES,        /* the 0x05 bytes scalar */
  MIZU_IX_VEC,          /* 0x06-0x0a, 0x0e: type is the MIZU_TYPE_* tag */
  MIZU_IX_STRV,         /* begin: count STR items follow */
  MIZU_IX_LIST,         /* begin: count value items follow */
  MIZU_IX_DICT,         /* begin: count (STR key, value) pairs follow */
  MIZU_IX_ATTR,         /* begin: one value item, then one DICT begin */
  MIZU_IX_ERR,          /* the err fields; legal at the top level only */
  MIZU_IX_TASK          /* header: count fields follow as ordinary items */
};

typedef struct mizu_ix_item_s {
  uint32_t kind;        /* MIZU_IX_* */
  uint32_t type;        /* VEC: the MIZU_TYPE_* wire type */
  int na;               /* STR1 / STR: the -1 (NA) form */
  int key;              /* STR: a dict key (an strv element has 0) */
  const unsigned char *ptr;  /* STR1 / STR / BYTES / VEC: the data span */
  uint64_t len;         /* STR1 / STR: the span's byte length */
  uint64_t count;       /* BYTES / VEC / STRV / LIST / DICT: the element
                           (pair) count; TASK: the field arity */
  uint64_t u64[2];      /* LGL: u64[0] in {0, 1, 2}; INT / REAL: u64[0];
                           CPLX: both; TASK: u64[0] = submitter identity */
  uint32_t target;      /* TASK: the target language byte */
  uint32_t task_kind;   /* TASK: the kind byte */
  uint32_t err_flags;   /* ERR: bit 0 = index present */
  uint64_t err_index;   /* ERR: valid when err_flags bit 0 is set */
  struct { const unsigned char *ptr; uint64_t len; } err_str[3];
                        /* ERR: type, message, detail */
} mizu_ix_item;

/* The pull cursor. Stack-allocated by the caller; the frame stack is the
   depth cap's accounting (LIST / DICT / STRV / ATTR / TASK frames, the
   dict's remaining counting keys and values alike). */
typedef struct mizu_ix_s {
  const unsigned char *p;
  const unsigned char *end;
  uint32_t depth;
  int done;             /* the root value completed */
  int err;              /* latched: all later calls fail */
  struct {
    uint32_t kind;      /* the MIZU_IX_* begin kind */
    uint64_t remaining; /* items owned at this level */
  } stack[MIZU_IX_DEPTH_MAX];
} mizu_ix;

/* Open a cursor over a whole stream: the magic and version checks, the
   unknown version taking the informative "the peer uses a newer format"
   decline. MIZU_OK, or MIZU_ERR with the error in the TLS slot. */
MIZU_API mizu_status mizu_ix_open(mizu_ix *cur, const void *buf, size_t len);
/* The next item: bounds, depth, UTF-8 and grammar checks per the spec.
   MIZU_OK and *item filled, or MIZU_ERR with the informative text in the
   TLS slot (an unknown tag or task kind declines as "the peer uses a
   newer format", never a bare corrupt-stream error). */
MIZU_API mizu_status mizu_ix_next(mizu_ix *cur, mizu_ix_item *item);
/* The finishing check: exactly one value per stream — fails on a
   truncated root value or on bytes past it. */
MIZU_API mizu_status mizu_ix_end(mizu_ix *cur);

/* The interop emit helpers' bodies (the interchange stream section
   below): every count and value follows its tag byte directly at
   unaligned offsets, little-endian, so writes are memcpy, never casts. */

MIZU_EXT_INLINE size_t mizu_ext_ix_put_header_impl(unsigned char *dst) {
  if (dst != NULL) {
    dst[0] = (unsigned char) MIZU_INTEROP_MAGIC;
    dst[1] = (unsigned char) MIZU_IX_VERSION;
  }
  return 2;
}

MIZU_EXT_INLINE size_t mizu_ext_ix_put_tag8_impl(unsigned char *dst,
                                                uint32_t tag, uint64_t v) {
  /* tag + one trailing u64: the vector/container/bytes wire shape */
  if (dst != NULL) {
    dst[0] = (unsigned char) tag;
    memcpy(dst + 1, &v, 8);
  }
  return 9;
}

MIZU_EXT_INLINE size_t mizu_ext_ix_put_nil_impl(unsigned char *dst) {
  if (dst != NULL) dst[0] = MIZU_IX_TAG_NIL;
  return 1;
}

MIZU_EXT_INLINE size_t mizu_ext_ix_put_lgl_impl(unsigned char *dst,
                                               int value) {
  if (dst != NULL) {
    dst[0] = MIZU_IX_TAG_LGL1;
    dst[1] = (unsigned char) value;
  }
  return 2;
}

MIZU_EXT_INLINE size_t mizu_ext_ix_put_int_impl(unsigned char *dst,
                                               int64_t value) {
  if (dst != NULL) {
    dst[0] = MIZU_IX_TAG_INT;
    memcpy(dst + 1, &value, 8);
  }
  return 9;
}

MIZU_EXT_INLINE size_t mizu_ext_ix_put_real_impl(unsigned char *dst,
                                                double value) {
  if (dst != NULL) {
    dst[0] = MIZU_IX_TAG_REAL;
    memcpy(dst + 1, &value, 8);
  }
  return 9;
}

MIZU_EXT_INLINE size_t mizu_ext_ix_put_cplx_impl(unsigned char *dst,
                                                double re, double im) {
  if (dst != NULL) {
    dst[0] = MIZU_IX_TAG_CPLX;
    memcpy(dst + 1, &re, 8);
    memcpy(dst + 9, &im, 8);
  }
  return 17;
}

MIZU_EXT_INLINE size_t mizu_ext_ix_put_str_impl(unsigned char *dst,
                                               const void *s, int32_t len) {
  if (dst != NULL) {
    dst[0] = MIZU_IX_TAG_STR;
    memcpy(dst + 1, &len, 4);
    if (len > 0) memcpy(dst + 5, s, (size_t) len);
  }
  return (size_t) 5 + (len > 0 ? (size_t) len : 0);
}

MIZU_EXT_INLINE size_t mizu_ext_ix_put_bytes_impl(unsigned char *dst,
                                                 const void *data,
                                                 uint64_t count) {
  if (dst != NULL) {
    dst[0] = MIZU_IX_TAG_BYTES;
    memcpy(dst + 1, &count, 8);
    if (count != 0) memcpy(dst + 9, data, (size_t) count);
  }
  return (size_t) 9 + (size_t) count;
}

MIZU_EXT_INLINE size_t mizu_ext_ix_put_vec_impl(unsigned char *dst,
                                               int wire_type,
                                               const void *data,
                                               uint64_t count) {
  uint32_t tag;
  switch (wire_type) {
  case MIZU_TYPE_LGL:  tag = MIZU_IX_TAG_LGLV;  break;
  case MIZU_TYPE_INT:  tag = MIZU_IX_TAG_INTV;  break;
  case MIZU_TYPE_REAL: tag = MIZU_IX_TAG_REALV; break;
  case MIZU_TYPE_CPLX: tag = MIZU_IX_TAG_CPLXV; break;
  case MIZU_TYPE_RAW:  tag = MIZU_IX_TAG_RAWV;  break;
  case MIZU_TYPE_INT64: tag = MIZU_IX_TAG_I64V; break;
  default: return 0;
  }
  const size_t elt = mizu_type_elt_size(wire_type);
  if (dst != NULL) {
    dst[0] = (unsigned char) tag;
    memcpy(dst + 1, &count, 8);
    if (count != 0) memcpy(dst + 9, data, (size_t) count * elt);
  }
  return (size_t) 9 + (size_t) count * elt;
}

MIZU_EXT_INLINE size_t mizu_ext_ix_put_strv_begin_impl(unsigned char *dst,
                                                      uint64_t count) {
  return mizu_ext_ix_put_tag8_impl(dst, MIZU_IX_TAG_STRV, count);
}

MIZU_EXT_INLINE size_t mizu_ext_ix_put_strelt_impl(unsigned char *dst,
                                                  const void *s,
                                                  int32_t len) {
  if (dst != NULL) {
    memcpy(dst, &len, 4);
    if (len > 0) memcpy(dst + 4, s, (size_t) len);
  }
  return (size_t) 4 + (len > 0 ? (size_t) len : 0);
}

MIZU_EXT_INLINE size_t mizu_ext_ix_put_list_begin_impl(unsigned char *dst,
                                                      uint64_t count) {
  return mizu_ext_ix_put_tag8_impl(dst, MIZU_IX_TAG_LIST, count);
}

MIZU_EXT_INLINE size_t mizu_ext_ix_put_dict_begin_impl(unsigned char *dst,
                                                      uint64_t count) {
  return mizu_ext_ix_put_tag8_impl(dst, MIZU_IX_TAG_DICT, count);
}

MIZU_EXT_INLINE size_t mizu_ext_ix_put_key_impl(unsigned char *dst,
                                               const void *s, uint32_t len) {
  if (dst != NULL) {
    memcpy(dst, &len, 4);
    if (len != 0) memcpy(dst + 4, s, (size_t) len);
  }
  return (size_t) 4 + (size_t) len;
}

MIZU_EXT_INLINE size_t mizu_ext_ix_put_attr_impl(unsigned char *dst) {
  if (dst != NULL) dst[0] = MIZU_IX_TAG_ATTR;
  return 1;
}

MIZU_EXT_INLINE size_t mizu_ext_ix_put_err_impl(unsigned char *dst,
                                               int has_index, uint64_t index,
                                               const void *type,
                                               uint32_t type_len,
                                               const void *message,
                                               uint32_t message_len,
                                               const void *detail,
                                               uint32_t detail_len) {
  if (dst != NULL) {
    const uint16_t flags = (uint16_t) (has_index != 0);
    dst[0] = MIZU_IX_TAG_ERR;
    memcpy(dst + 1, &flags, 2);
    size_t off = 3;
    if (has_index) {
      memcpy(dst + off, &index, 8);
      off += 8;
    }
    const void *strs[3] = { type, message, detail };
    const uint32_t lens[3] = { type_len, message_len, detail_len };
    for (int i = 0; i < 3; i++) {
      memcpy(dst + off, &lens[i], 4);
      off += 4;
      if (lens[i] != 0) {
        memcpy(dst + off, strs[i], (size_t) lens[i]);
        off += (size_t) lens[i];
      }
    }
    return off;
  }
  return (size_t) 3 + (has_index ? 8 : 0) +
         (size_t) 12 + (size_t) type_len + (size_t) message_len +
         (size_t) detail_len;
}

MIZU_EXT_INLINE size_t mizu_ext_ix_put_task_impl(unsigned char *dst,
                                                int target, int kind,
                                                uint64_t ident) {
  if (dst != NULL) {
    const uint16_t zero = 0;
    dst[0] = MIZU_IX_TAG_TASK;
    dst[1] = (unsigned char) target;
    dst[2] = (unsigned char) kind;
    memcpy(dst + 3, &zero, 2);
    memcpy(dst + 5, &ident, 8);
  }
  return 13;
}

#ifdef MIZU_EXT_NO_INLINES
MIZU_API size_t mizu_ix_put_header(unsigned char *dst);
MIZU_API size_t mizu_ix_put_nil(unsigned char *dst);
MIZU_API size_t mizu_ix_put_lgl(unsigned char *dst, int value);
MIZU_API size_t mizu_ix_put_int(unsigned char *dst, int64_t value);
MIZU_API size_t mizu_ix_put_real(unsigned char *dst, double value);
MIZU_API size_t mizu_ix_put_cplx(unsigned char *dst, double re, double im);
MIZU_API size_t mizu_ix_put_str(unsigned char *dst, const void *s,
                              int32_t len);
MIZU_API size_t mizu_ix_put_bytes(unsigned char *dst, const void *data,
                                uint64_t count);
MIZU_API size_t mizu_ix_put_vec(unsigned char *dst, int wire_type,
                              const void *data, uint64_t count);
MIZU_API size_t mizu_ix_put_strv_begin(unsigned char *dst, uint64_t count);
MIZU_API size_t mizu_ix_put_strelt(unsigned char *dst, const void *s,
                                 int32_t len);
MIZU_API size_t mizu_ix_put_list_begin(unsigned char *dst, uint64_t count);
MIZU_API size_t mizu_ix_put_dict_begin(unsigned char *dst, uint64_t count);
MIZU_API size_t mizu_ix_put_key(unsigned char *dst, const void *s,
                              uint32_t len);
MIZU_API size_t mizu_ix_put_attr(unsigned char *dst);
MIZU_API size_t mizu_ix_put_err(unsigned char *dst, int has_index,
                              uint64_t index,
                              const void *type, uint32_t type_len,
                              const void *message, uint32_t message_len,
                              const void *detail, uint32_t detail_len);
MIZU_API size_t mizu_ix_put_task(unsigned char *dst, int target, int kind,
                               uint64_t ident);
#else
/* The emit helpers: each returns its byte count, writing only when dst
   is not NULL, so a binding's two-pass walk sizes (dst NULL) and writes
   through the same byte-level code. The write pass relies on the size
   pass's count, so no limit is carried. Bodies are impl delegations, the
   dual-form single-sourcing discipline. */
MIZU_EXT_INLINE size_t mizu_ix_put_header(unsigned char *dst) {
  return mizu_ext_ix_put_header_impl(dst);
}
MIZU_EXT_INLINE size_t mizu_ix_put_nil(unsigned char *dst) {
  return mizu_ext_ix_put_nil_impl(dst);
}
/* value on the wire: 0, 1, or 2 (NA). */
MIZU_EXT_INLINE size_t mizu_ix_put_lgl(unsigned char *dst, int value) {
  return mizu_ext_ix_put_lgl_impl(dst, value);
}
MIZU_EXT_INLINE size_t mizu_ix_put_int(unsigned char *dst, int64_t value) {
  return mizu_ext_ix_put_int_impl(dst, value);
}
/* real and cplx take doubles and memcpy them: NaN payloads are
   bitwise-preserved. */
MIZU_EXT_INLINE size_t mizu_ix_put_real(unsigned char *dst, double value) {
  return mizu_ext_ix_put_real_impl(dst, value);
}
MIZU_EXT_INLINE size_t mizu_ix_put_cplx(unsigned char *dst, double re,
                                      double im) {
  return mizu_ext_ix_put_cplx_impl(dst, re, im);
}
/* The 0x04 string scalar; len -1 is NA (s may then be NULL). */
MIZU_EXT_INLINE size_t mizu_ix_put_str(unsigned char *dst, const void *s,
                                     int32_t len) {
  return mizu_ext_ix_put_str_impl(dst, s, len);
}
MIZU_EXT_INLINE size_t mizu_ix_put_bytes(unsigned char *dst,
                                       const void *data, uint64_t count) {
  return mizu_ext_ix_put_bytes_impl(dst, data, count);
}
/* The fixed-width vector tags: wire_type one of MIZU_TYPE_LGL / INT /
   REAL / CPLX / RAW / INT64 (any other returns 0 — the bindings pass
   only the six). */
MIZU_EXT_INLINE size_t mizu_ix_put_vec(unsigned char *dst, int wire_type,
                                     const void *data, uint64_t count) {
  return mizu_ext_ix_put_vec_impl(dst, wire_type, data, count);
}
MIZU_EXT_INLINE size_t mizu_ix_put_strv_begin(unsigned char *dst,
                                            uint64_t count) {
  return mizu_ext_ix_put_strv_begin_impl(dst, count);
}
/* A bare strv element; len -1 is NA. */
MIZU_EXT_INLINE size_t mizu_ix_put_strelt(unsigned char *dst, const void *s,
                                        int32_t len) {
  return mizu_ext_ix_put_strelt_impl(dst, s, len);
}
MIZU_EXT_INLINE size_t mizu_ix_put_list_begin(unsigned char *dst,
                                            uint64_t count) {
  return mizu_ext_ix_put_list_begin_impl(dst, count);
}
MIZU_EXT_INLINE size_t mizu_ix_put_dict_begin(unsigned char *dst,
                                            uint64_t count) {
  return mizu_ext_ix_put_dict_begin_impl(dst, count);
}
/* A bare dict key: never NA, so the length is unsigned. */
MIZU_EXT_INLINE size_t mizu_ix_put_key(unsigned char *dst, const void *s,
                                     uint32_t len) {
  return mizu_ext_ix_put_key_impl(dst, s, len);
}
MIZU_EXT_INLINE size_t mizu_ix_put_attr(unsigned char *dst) {
  return mizu_ext_ix_put_attr_impl(dst);
}
MIZU_EXT_INLINE size_t mizu_ix_put_err(unsigned char *dst, int has_index,
                                     uint64_t index,
                                     const void *type, uint32_t type_len,
                                     const void *message,
                                     uint32_t message_len,
                                     const void *detail,
                                     uint32_t detail_len) {
  return mizu_ext_ix_put_err_impl(dst, has_index, index, type, type_len,
                                 message, message_len, detail, detail_len);
}
/* The task header: target language byte, kind byte, the reserved u16
   flags written zero, then the submitter identity. */
MIZU_EXT_INLINE size_t mizu_ix_put_task(unsigned char *dst, int target,
                                      int kind, uint64_t ident) {
  return mizu_ext_ix_put_task_impl(dst, target, kind, ident);
}
#endif

// Utilities ----------------------------------------------------------------------

/* Monotonic seconds (the spin/deadline clock) and the current pid
   (cached, with an atfork reset). */
MIZU_API double mizu_now(void);
MIZU_API long mizu_self_pid(void);

/* Category + remediation text for an MIZU_ERRCAT (what the handle /
   thread-local error slot records). */
MIZU_API void mizu_err_describe(mizu_errcat, const char **summary,
                              const char **hint);

/* One 2^127-step L'Ecuyer-CMRG stream jump in place over a 6-word state
   (rng_jump.c; a binding's map derives per-element streams with it). */
MIZU_API void mizu_rng_jump(int *seed);

/* malloc tuning (tune.c), called by the binding at load. */
MIZU_API void mizu_tune(void);

#ifdef __cplusplus
}
#endif

#endif /* MIZU_EXT_H */
