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
   mizu_parker_snapshot, mizu_zc_rc, mizu_zc_flags_, and the wire helpers
   (mizu_timeout_ms, mizu_store_na_real, mizu_aux_rawspill_pool,
   mizu_aux_shm_vec, mizu_mizh_write, mizu_mizh_check) ship as
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
} mizu_binding;

/* Zero and size-stamp a binding struct. Call before filling the fn pointers. */
MIZU_API void mizu_binding_init(mizu_binding *);

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

/* The 128-byte region header. Not pool wire format — it rides its own
   region, keyed by the same ABI version — but the same rules apply: the
   struct is the layout, 64-byte-aligned sections follow it. */
typedef struct mizu_morsel_hdr_s {
  uint32_t magic;          /* the binding's tag */
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
MIZU_API uint64_t mizu_morsel_layout(mizu_morsel_hdr *h, uint32_t magic,
                                   uint64_t n, uint64_t morsel_size,
                                   uint64_t desc_len, uint32_t x_type,
                                   uint64_t x_len, uint32_t out_type,
                                   uint64_t out_m, uint32_t claim_n);
/* Validate a region's header against the mapping size and the binding's
   magic, including section placement. Returns NULL and fills *out (when
   non-NULL) on success, else a static message. Runs once per mapping. */
MIZU_API const char *mizu_morsel_hdr_check(const void *base, size_t size,
                                         uint32_t magic,
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
   kinds and its decode pair, and the flat MIZH header write/check. */

#ifdef MIZU_EXT_NO_INLINES
MIZU_API double mizu_timeout_ms(double seconds);
MIZU_API void mizu_store_na_real(void *dst);
MIZU_API uint64_t mizu_aux_rawspill_pool(int type, uint32_t name_len);
MIZU_API uint64_t mizu_aux_shm_vec(int type, uint64_t total);
MIZU_API int mizu_aux_type(uint64_t aux);
MIZU_API uint64_t mizu_aux_hi(uint64_t aux);
MIZU_API void mizu_mizh_write(void *base, int wire_type, int64_t n_elems);
MIZU_API int mizu_mizh_check(const void *base, size_t size,
                           int *wire_type, int64_t *n_elems);
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
   wire type, and the element/attribute extents against the region size —
   0 on success, -1 on any rejection. Attribute policy (whether the word
   at [16, 24) may be nonzero, and what it means) stays binding-side. */
MIZU_EXT_INLINE int mizu_mizh_check(const void *base, size_t size,
                                  int *wire_type, int64_t *n_elems) {
  if (size < MIZU_HEADER_SIZE) return -1;
  uint32_t magic;
  int32_t t32;
  int64_t len, attrs;
  memcpy(&magic, base, 4);
  if (magic != MIZU_MAGIC_VEC) return -1;
  memcpy(&t32, (const unsigned char *) base + 4, 4);
  memcpy(&len, (const unsigned char *) base + 8, 8);
  memcpy(&attrs, (const unsigned char *) base + 16, 8);
  const size_t elt = mizu_type_elt_size(t32);
  if (elt == 0 || len < 0 || attrs < 0 ||
      len > ((int64_t) size - (int64_t) MIZU_HEADER_SIZE) / (int64_t) elt ||
      attrs > (int64_t) size - (int64_t) MIZU_HEADER_SIZE -
              len * (int64_t) elt)
    return -1;
  *wire_type = t32;
  *n_elems = len;
  return 0;
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
