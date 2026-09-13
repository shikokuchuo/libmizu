/* rei_ext.h — librei binding-author API (the unstable tier)

   The sanctioned surface for language bindings and the built-in bytes
   binding: the callback seam, the stager/read/publish services, and the
   promoted internals the first-party bindings (rei for R, pyrei for
   Python) compile against.

   Tier policy: this header is version-pinned per minor release. Any
   declaration, struct layout, or macro here may change on a minor
   version bump, without deprecation — including after rei.h's stable
   promise starts at 1.0. Consumers rebuild against each librei minor
   release (the first-party bindings vendor the sources at a pinned
   commit). rei.h carries the stable-ABI promise; src/internal.h stays
   private. There is no third state: a symbol not here or in rei.h is
   private.

   Conventions differ from rei.h where the consumers differ: this tier
   is C-only binding surface, so macros are fine, and the hot accessors
   plus the small wire-format helpers ship in dual form (below). Every
   other operation is a real exported function (REI_API), so FFI
   consumers see the full tier.

   Dual-form fast paths (the CPython Py_INCREF pattern):
   rei_parker_snapshot, rei_zc_rc, rei_zc_flags_, and the wire helpers
   (rei_timeout_ms, rei_store_na_real, rei_aux_rawspill_pool,
   rei_aux_shm_vec, rei_reih_write, rei_reih_check) ship as
   `static inline` here AND as same-named exported functions (src/ext.c).
   A C TU inlines its own copy — zero cost — while an FFI binds the
   exported symbol. This is legal C: internal and external linkage of the
   same name in different TUs do not clash; taking the address in an
   ordinary TU yields the local inline's address, which is harmless (the
   accessors are pure offset math over the wire-format offsets). A TU
   defining the exported forms defines REI_EXT_NO_INLINES before including
   this header: a static inline and an extern definition of one name
   cannot coexist in a TU. Single-sourcing: both forms are thin wrappers
   over the same offset macros / struct definitions below — the macros,
   not the bodies, are the one definition. */

#ifndef REI_EXT_H
#define REI_EXT_H

#include <math.h>
#include <string.h>

#include "rei.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Some static inlines below go unused in some TUs; the amalgamation
   folds this header into the single rei.c TU, where they would trip
   -Wunused-function (header inlines are exempt only in a real header). */
#if defined(__GNUC__) || defined(__clang__)
#  define REI_EXT_INLINE static inline __attribute__((unused))
#else
#  define REI_EXT_INLINE static inline
#endif

/* The dual-form inlines touch atomics; spell the acquire load per
   language (rei.h's REI_ATOMIC gives C++ consumers std::atomic). */
#ifdef __cplusplus
#  define REI_EXT_LOAD_ACQ(p) \
     std::atomic_load_explicit((p), std::memory_order_acquire)
#else
#  define REI_EXT_LOAD_ACQ(p) \
     atomic_load_explicit((p), memory_order_acquire)
#endif

// Binding-tier constants ---------------------------------------------------------

#ifdef _WIN32
#  define REI_PREFIX_LITERAL "Local\\rei_"
#else
#  define REI_PREFIX_LITERAL "/rei_"
#endif

#define REI_ALIGN64(x) (((x) + 63) & ~(size_t) 63)

/* Staging-policy floors for a binding's stage_fn (not used by the
   core): SHM_VEC escalates only past max(inline budget, REI_ZC_FLOOR);
   the channel's raw floor is higher (the arena copy has no region
   machinery to amortize) and lifts entirely under the churn signal.
   Bindings pick their own. */
#define REI_ZC_FLOOR     ((size_t) 32768)
#define REI_ZC_FLOOR_RAW ((size_t) (256 << 10))

/* The core's consumer SHM_RAW mapping cache size; a binding sizes its
   own view cache to match. */
#define REI_OPEN_CACHE_MAX  16

/* The self-describing stream dispatch byte: payload byte 0 of an INLINE
   frame, where an R native stream carries 'B'/'X'. The core is
   codec-agnostic; DESIGN.md's codec registry allocates the magic bytes
   ('R' = rei, 'P' = pyrei). 'R' here and REI_DROP_R share the letter
   deliberately — both denote an R-binding payload, in disjoint
   contexts. */
#define REI_CODEC_MAGIC 0x52u   /* 'R' */
#define REI_PYREI_CODEC_MAGIC 0x50u   /* 'P' */

// Handle views -------------------------------------------------------------------

/* Generic handle view for the stager services; the core passes the
   handle a binding's callbacks were registered with. Never constructed
   by bindings; the struct definition stays private (src/internal.h). */
typedef struct rei_handle_s rei_handle;
/* Valid only during an exec callback. The struct definition is below
   (the ERR-envelope frame buffer is binding surface). */
typedef struct rei_result_sink_s rei_result_sink;

/* Handle-kind discriminator. The staging services are kind-specific:
   rei_stage_arena_alloc / rei_stage_reap are channel-only. */
enum { REI_HTYPE_CHANNEL = 1, REI_HTYPE_POOL = 2 };

/* Handle queries standing in for field access into the private handle
   struct. kind returns REI_HTYPE_*. churn reads the spill-churn signal
   (raised on Linux only: a spill pop missed with lent regions
   outstanding; cleared when a ledger sweep or force-reclaim returns one)
   — while set, a stager falls back to the copy tiers. The churn read is
   an extern call: gate it behind the payload's size check so it runs
   only for payloads already proven large. binding_ctx returns the
   binding.ctx pointer registered at create/attach/join (the seam
   callbacks receive it directly — this is for contexts with no callback
   in flight). spill_info fills the producer free-list entry count and
   the lent-region ledger count (debug/dump surface; the per-kind public
   snapshots are rei_channel_info_get / rei_pool_dump_get). */
REI_API int rei_handle_kind(const rei_handle *);
REI_API int rei_handle_churn(const rei_handle *);
/* Superseded on the stage path (stage_fn receives the ctx directly since
   0.3.0); retained for contexts with no seam callback in flight. */
REI_API void *rei_handle_binding_ctx(const rei_handle *);
REI_API void rei_handle_spill_info(const rei_handle *,
                                   uint32_t *fl_entries,
                                   uint32_t *ledger_entries);

// The binding seam ---------------------------------------------------------------

/* The core never sees a language object. A binding registers these
   callbacks at create/attach/join; the core copies them into the handle,
   so a hot-path call is one load + a predicted indirect branch.

   stage: frame obj as (hdr, payload) — payload capacity inline_max.
     Spill/arena/retain via the rei_stage_* services on the handle. ctx
     is the handle's binding.ctx. Returns 0 on success, nonzero on
     staging failure (the verb fails as REI_ERR / REI_ERRCAT_STAGE). May
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
     ctx->outcome carries the result's terminal state (REI_RS_OK/ERR
     with a payload frame; REI_RS_CANCEL/DIED without one — read_fn
     builds the binding's error object for all three). A vanished
     out-of-line region: rei_read_region sets ctx->gone and read_fn
     propagates by returning NULL. A NULL return consumes nothing —
     unless the read first set ctx->flags |= REI_READ_CONSUME: then the
     transport consumes exactly as on success (channel: the head advance
     and its batched publication; pool OK/ERR: the FREE transition, the
     task-keeper drop, and the producer-keeper wake) while the verb
     still returns REI_ERR. Use for foreign or corrupt payloads that
     must not wedge the ring behind an unreadable slot: the binding
     carries its specific message in its own state (the core records no
     generic error for a consumed read), and a retried read sees the
     next slot.
   exec: pool workers only — run one claimed task frame and publish
     through the sink. ctx is a read ctx on the handle (outcome
     REI_RS_OK): decode reads ride it (rei_read_region, ctx->gone), and
     the binding ctx remains reachable as ctx->binding_ctx. Must not
     abandon: the binding catches every task
     condition into the sink; an escape degrades to worker death plus
     the reaper verdict (the hard-crash semantics, never the path for an
     ordinary task error). catching marks a reentrant invocation
     (nested-collect help, nested submit's inline execute): contain task
     conditions there instead of letting them unwind through the worker
     loop. Returns 0 on success; nonzero is infrastructure failure and
     takes the worker down (REI_EXIT_ERROR).
   check: interrupt/cancel poll, invoked from core wait/work loops at
     abandon-safe points only (no shared-state mutation in progress, no
     cleanup pending), once per spin/park iteration. Return 0 to
     continue, nonzero to abandon: the verb unwinds as REI_ERR with
     REI_ERRCAT_INTERRUPTED, consuming nothing (a recv interrupted
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
     stager registered with rei_stage_pin. Fires only on the
     handle-owning thread. NULL when the binding never pins.
   ctx: opaque to the core. */

typedef int (*rei_stage_fn)(void *obj, rei_slot_hdr *hdr,
                            uint8_t *payload, uint32_t inline_max,
                            rei_handle *, void *ctx);

/* read_fn flags (ctx->flags, zero at each read's start): CONSUME makes a
   NULL return consume the slot exactly as on success while the verb
   still returns REI_ERR — the foreign/corrupt-payload contract; see the
   seam comment above. */
#define REI_READ_CONSUME 1u

typedef struct rei_read_ctx_s {
  uint32_t size;        /* core-set: sizeof the struct it knows */
  int32_t outcome;      /* pool: rei_rs_status of the result (channel:
                           REI_RS_OK) */
  int32_t gone;         /* set by rei_read_region on a vanished region;
                           read_fn propagates by returning NULL */
  int32_t died_slot;    /* REI_RS_DIED: the claimant worker slot (-1) */
  int64_t died_pid;     /* REI_RS_DIED: its pid (0 when unknown) */
  rei_handle *handle;   /* the reading handle */
  void   *binding_ctx;  /* the handle's binding.ctx */
  uint32_t flags;       /* binding-set REI_READ_*; zero on entry */
  void   *reserved[4];  /* zero; future growth without a soname bump */
} rei_read_ctx;

typedef void *(*rei_read_fn)(const rei_slot_hdr *, const uint8_t *payload,
                             size_t limit, rei_read_ctx *);

typedef int (*rei_exec_fn)(const rei_slot_hdr *hdr, const uint8_t *payload,
                           size_t limit, rei_result_sink *, int catching,
                           rei_read_ctx *ctx);
typedef int (*rei_check_fn)(void *ctx);
typedef void (*rei_park_fn)(void *ctx, int entering);
typedef void (*rei_sweep_fn)(void *ctx);
typedef void (*rei_drop_fn)(void *ctx, void *pin);

typedef struct rei_binding_s {
  uint32_t     size;    /* sizeof(rei_binding); set via rei_binding_init */
  rei_stage_fn stage;
  rei_read_fn  read;
  rei_exec_fn  exec;    /* pool workers only; NULL on submitter handles */
  rei_check_fn check;   /* interrupt poll; NULL for plain-C consumers */
  rei_park_fn  park;    /* around-park lock release; usually NULL */
  rei_sweep_fn sweep;   /* idle cache drop; usually NULL */
  rei_drop_fn  drop;    /* pin release; NULL when the binding never pins */
  void        *ctx;     /* opaque to the core */
} rei_binding;

/* Zero and size-stamp a binding struct. Call before filling the fn pointers. */
REI_API void rei_binding_init(rei_binding *);

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
REI_API void *rei_stage_arena_alloc(rei_handle *, size_t n, uint64_t *off);
REI_API rei_status rei_stage_spill_get(rei_handle *, size_t n,
                                       rei_shm **out);
REI_API void rei_stage_retain(rei_handle *, rei_shm *region);
REI_API void rei_stage_retain_zc(rei_handle *, rei_shm *region);
REI_API void rei_stage_pin(rei_handle *, void *pin);
REI_API void rei_stage_reap(rei_handle *);

/* Raw-tier reservation (stage_raw.c), valid only during stage_fn: choose
   the tier for n bare bytes of wire_type and return the destination with
   hdr stamped — the caller memcpys n bytes into it. NULL hands the object
   to the binding's serialized tiers (a reservation failure degrades, never
   errors). Policy:
   n <= inline_max → RAWVEC inline (aux = wire_type).
   channel: arena RAWSPILL (aux = wire_type, payload = chunk offset) when
     n <= REI_ZC_FLOOR_RAW or under churn; else flat SHM_VEC (REIH header +
     retain_zc, aux = wire_type | exact used bytes << 8, payload = region
     name) when n >= max(inline_max, REI_ZC_FLOOR) and no churn, reaping
     before the checkout; an arena retry on region failure; else NULL.
   pool: flat SHM_VEC on the same zc gate; else a RAWSPILL region (aux =
     wire_type | name_len << 8, payload = name; n <= UINT32_MAX); else NULL.
   Bare bytes carry no identifier, so nothing is pinned. Eligibility probes
   (which objects are raw), STR1/NIL/REF, and the string/list-tree SHM_VEC
   layouts stay binding-side. Dual form (see the banner): the inline stamps
   the RAWVEC fast path and falls through to the exported slow path. */
REI_API void *rei_stage_raw_spill(rei_handle *, uint64_t n, int wire_type,
                                  rei_slot_hdr *, uint8_t *payload,
                                  uint32_t inline_max);
#ifdef REI_EXT_NO_INLINES
REI_API void *rei_stage_raw(rei_handle *, uint64_t n, int wire_type,
                            rei_slot_hdr *, uint8_t *payload,
                            uint32_t inline_max);
#else
REI_EXT_INLINE void *rei_stage_raw(rei_handle *h, uint64_t n, int wire_type,
                                   rei_slot_hdr *hdr, uint8_t *payload,
                                   uint32_t inline_max) {
  if (n <= (uint64_t) inline_max) {
    hdr->kind = REI_KIND_RAWVEC;
    hdr->len = (uint32_t) n;
    hdr->aux = (uint64_t) (uint32_t) wire_type;
    return payload;
  }
  return rei_stage_raw_spill(h, n, wire_type, hdr, payload, inline_max);
}
#endif

/* Read-side service, invoked through the read_ctx handed to read_fn: a
   borrowed consumer mapping for a SHM_RAW-class payload name, from the
   handle's open cache (open/fstat/mmap on a miss, LRU-evicted). The
   mapping is cache-owned: copy the payload out before consumer-done. On
   a vanished region, sets ctx->gone and returns NULL; read_fn
   propagates by returning NULL. View tiers do not use it — their
   mappings are binding-owned (rei_shm_open_view), pinned by the view. */
REI_API rei_shm *rei_read_region(rei_read_ctx *, const uint8_t *name,
                                 uint32_t len);

/* Result publish, valid during exec only. publish stages value via the
   worker handle's stage_fn and publishes REI_RS_OK — a result past the
   inline budget spills to a region whose consumer is the submitter, so
   a large result never has to fit the slot. publish_err publishes
   REI_RS_ERR: when inline_n != 0 the binding has already framed the
   flattened envelope INLINE in the sink's frame buffer (a self-contained
   stream pins nothing); otherwise the flattened object rides the tiered
   stage (the tiny-slot fallback). The ERR envelope is INLINE-framed by
   construction wherever a classed condition fits the slot, so the
   publish cannot fail: a task error fails the task, never the worker.
   publish_died is the status-only terminal for a task whose out-of-line
   payload vanished with its dead enqueuer. The publish pair return 1 when
   the publish CAS won, 0 when a cancel beat it, and -1 on infrastructure
   failure (recorded on the handle — the worker cannot continue). */
REI_API int rei_result_publish(rei_result_sink *, void *value);
REI_API int rei_result_publish_err(rei_result_sink *, void *flattened,
                                   uint32_t inline_n);
REI_API void rei_result_publish_died(rei_result_sink *);

// The built-in bytes binding -----------------------------------------------------

/* A byte buffer. Send: the consumer fills {data, len} and passes its
   address as the verb's obj. Recv: the binding returns a malloc'd
   rei_bytes (data rides the same allocation); release with
   rei_bytes_free — an export, not documented free(), so a shared-library
   build never crosses allocator domains. Fixed layout, never extended. */
typedef struct rei_bytes_s {
  void *data;
  size_t len;
} rei_bytes;

/* Fill `binding` with the bytes binding: stage/read only — exec, check,
   park, sweep, drop are NULL (a bytes handle is a channel peer or a pool
   submitter, never a worker). Stage rides the INLINE/ARENA/SHM_RAW tiers
   exactly as a serialize stream does; read copies out before
   consumer-done. The copy tiers of a foreign peer (RAWVEC/RAWSPILL/STR1)
   read as their bare bytes; the view tiers (SHM_VEC/REF) fail the read —
   they are binding-owned by design. Pool collects surface non-OK
   outcomes only as the verb's rei_status — the binding builds no error
   object. This is the FFI zero-callback path, the reference stager for
   binding authors, and the C test tiers' stager. */
REI_API void rei_binding_bytes(rei_binding *);
REI_API void rei_bytes_free(rei_bytes *);

// The region handle struct ---------------------------------------------------------

/* The public opaque type in rei.h; defined here for bindings (the
   core's control-region mappings embed one in the handle structs).
   Layout-pinned per minor release like the rest of this tier: any field
   addition or reorder forces a binding rebuild. addr/size/name are
   borrowed reads, valid until close. */
struct rei_shm_s {
  void *addr;
  size_t size;
  char name[REI_NAME_MAX];
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

typedef struct rei_parker_s {
  REI_ATOMIC(uint32_t) *epoch;   /* in the shared region */
#ifdef _WIN32
  void *event;                   /* named auto-reset event handle */
#endif
} rei_parker;

typedef enum rei_park_result_e { REI_PARK_WOKEN = 0, REI_PARK_TIMEOUT,
                                 REI_PARK_INTR } rei_park_result;

/* region_name/entity name the Windows event ("<region>.pk.<entity>"),
   created by the region's host (create = 1) and opened by name by
   attachers; unused on POSIX. Returns 0 on success. */
REI_API int rei_parker_attach(rei_parker *pk, REI_ATOMIC(uint32_t) *epoch,
                              const char *region_name, int entity,
                              int create);
REI_API void rei_parker_detach(rei_parker *pk);

/* Sleeps while the epoch still equals snapshot, up to timeout_ms
   (0 = poll: never sleeps; < 0 = indefinite — on POSIX an untimed wait
   would silently restart under SA_RESTART and swallow an interrupt until
   the next genuine wake, so indefinite parks are internally bounded and
   rely on directed unparks). Returns REI_PARK_*. */
REI_API int rei_park(rei_parker *pk, uint32_t snapshot, long timeout_ms);
REI_API void rei_unpark(rei_parker *pk);

#ifdef REI_EXT_NO_INLINES
REI_API uint32_t rei_parker_snapshot(const rei_parker *pk);
#else
/* The park handshake's first step (dual form — see the banner). */
REI_EXT_INLINE uint32_t rei_parker_snapshot(const rei_parker *pk) {
  return REI_EXT_LOAD_ACQ(pk->epoch);
}
#endif

// Death watch (wait_linux.c / wait_macos.c / wait_win32.c) ------------------------

/* Translates a watched pid's exit into *flag = 1 plus a directed unpark
   of pk (optional, copied). A pid already dead fires immediately. The
   flag target and the parker's epoch word / event must stay valid until
   rei_death_watch_stop returns: stop synchronizes with any in-flight
   callback, so after it returns nothing touches them. Detection is a
   wake trigger only — the liveness lock is the verdict; pid-reuse races
   are absorbed there. Callback threading: the core's own reap watch
   (internal start2) fires on OS listener threads; start's flag store +
   unpark is the whole contract here. */

typedef struct rei_death_watch_s rei_death_watch;

REI_API rei_death_watch *rei_death_watch_start(long pid,
                                               REI_ATOMIC(int) *flag,
                                               const rei_parker *pk);
REI_API void rei_death_watch_stop(rei_death_watch *w);

/* Library-unload teardown; joins the Linux epoll thread (no-op
   elsewhere: macOS dispatch sources and Windows thread-pool waits are
   per-watch). */
REI_API void rei_death_listener_teardown(void);

// Liveness lock (liveness.c) ---------------------------------------------------

/* Exclusive flock (POSIX) / LockFileEx (Windows) held for a process's
   entire lifetime and released by the kernel on any exit path.
   fd-scoped, not PID-scoped: pid reuse cannot fake "alive". A probe is a
   non-blocking acquire on the fd kept from open — ACQUIRED means the
   previous holder is dead (and the caller now holds the lock,
   serializing survivor cleanup); HELD means alive. */

typedef enum rei_live_probe_e { REI_LIVE_ACQUIRED = 0,
                                REI_LIVE_HELD = 1 } rei_live_probe;

/* Directory for liveness lock files: the REI_LIVENESS_DIR override
   (read-through, checked every call) else a per-platform default
   resolved once — /dev/shm on Linux, the per-user temp dir on macOS and
   Windows. NULL if unresolvable. Only region creators call this;
   participants read the embedded copy. */
REI_API const char *rei_live_dir(void);

REI_API int rei_live_open(const char *path, intptr_t *out);
REI_API int rei_live_try(intptr_t h);
REI_API void rei_live_close(intptr_t h);

// Preamble (preamble.c) ----------------------------------------------------------

/* Host writes at create, immutable thereafter; the peer validates before
   any shared atomic is read or written. validate returns NULL and fills
   *out on success, else a static error message. The pool header check
   (magic + version) lives in the pool attach path, not here. */
REI_API void rei_preamble_write(void *region, const rei_preamble *p);
REI_API const char *rei_preamble_validate(const void *region,
                                          size_t region_size,
                                          rei_preamble *out);

// Map support (pool.c; a binding's map rides these) ---------------------------------

/* The opaque pool-signal trio a map runner loads relaxed once per batch
   transition: the help_wanted doorbell, the pool's shared shutdown word,
   and the handle's process-local listener-written owner_dead flag.
   Borrowed; the binding's map dereferences the words and stays
   pool-layout-free. */
typedef struct rei_pool_sig_s {
  REI_ATOMIC(uint32_t) *help_wanted;
  REI_ATOMIC(uint32_t) *shutdown;
  REI_ATOMIC(int)      *owner_dead;
} rei_pool_sig;

/* A malloc'd copy of the signal trio (the caller frees). NULL on
   failure. */
REI_API rei_pool_sig *rei_pool_signals(rei_pool *);
/* One doorbell help beat (claims a map runner and re-homes it onto the
   helper's own deque) and the test-harness injection pull. help_once
   returns 1 when it claimed, 0 when not, -1 on an exec_fn
   infrastructure failure (recorded on the handle). */
REI_API int rei_pool_help_once(rei_pool *);
REI_API int rei_pool_deque_pull(rei_pool *, uint32_t n);
/* A map's batch-sizing inputs: claims the worker's nested-submitter slot
   when unclaimed (as a first nested submit does), then reports the
   caller's FREE result slots, the injection cap, and the entry inline
   budget. Only this process allocates from its own subrange, so the
   FREE count can only grow under it. Returns -1 on a claim failure (the
   handle error slot holds it). */
REI_API int rei_pool_map_caps(rei_pool *, uint32_t *free_rs,
                              uint32_t *inj_cap, uint32_t *inline_entry);
/* The map runner's submit: rei_pool_submit plus the entry flags
   (REI_ENTRY_RUNNER marks a map's join ticket — a doorbell help beat
   re-homes it onto the helper's own deque instead of executing it). */
REI_API rei_status rei_pool_submit_flags(rei_pool *, void *task_obj,
                                         uint16_t flags, rei_task *out,
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
     arithmetic over them (rei_morsel_lost).
   The descriptor codec, the x-section element I/O, the batch loop, and the
   result gather stay binding-side. */

#define REI_MORSEL_CANCEL_OFF ((uint64_t) 0)
#define REI_MORSEL_GEN_OFF    ((uint64_t) 4)
#define REI_MORSEL_CURSOR_OFF ((uint64_t) 64)
#define REI_MORSEL_CLAIM_OFF  ((uint64_t) 128)
/* Generation comparisons mask to the CLAIM word's 30 bits (wrap takes 2^30
   resets of one region: harmless). */
#define REI_MORSEL_GEN_MASK   ((uint32_t) 0x3FFFFFFF)

enum { REI_MORSEL_IDLE = 0, REI_MORSEL_RUNNING, REI_MORSEL_ABANDONED };
enum { REI_MORSEL_X_DESC = 0, REI_MORSEL_X_RAW };

/* Batch sizing policy (rei_morsel_next): k targets a batch duration,
   growing at most 2x per step and shrinking immediately on overshoot,
   clamped to the cap — which bounds lost-set coarseness and the ramp worst
   case. Frozen by rei's 2026-08-04 gate sweep (M4 Pro, W = 4). */
#define REI_MORSEL_T_TARGET  200e-6
#define REI_MORSEL_BATCH_CAP 64

/* The 128-byte region header. Not pool wire format — it rides its own
   region, keyed by the same ABI version — but the same rules apply: the
   struct is the layout, 64-byte-aligned sections follow it. */
typedef struct rei_morsel_hdr_s {
  uint32_t magic;          /* the binding's tag */
  uint32_t version;        /* REI_ABI_VERSION */
  uint32_t flags;          /* reserved, 0 */
  uint32_t x_kind;         /* REI_MORSEL_X_* */
  uint32_t x_type;         /* raw x section wire type (REI_TYPE_*) */
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
} rei_morsel_hdr;
REI_STATIC_ASSERT(sizeof(rei_morsel_hdr) == 128,
                  "rei_morsel_hdr is the wire format");

/* Fill *h with the section layout for a map of n elements (morsel_size the
   geometry input, desc_len the descriptor stream size, x_type/x_len the
   optional raw x section — x_len must be exactly n elements — out_type/out_m
   the optional template output area, claim_n the CLAIM word count). Sections
   land 64-aligned in order: descriptor, x, morsel state, output. Returns the
   region size, 0 on invalid geometry or overflow (the binding raises its own
   error). The caller memcpys the header into the fresh region. */
REI_API uint64_t rei_morsel_layout(rei_morsel_hdr *h, uint32_t magic,
                                   uint64_t n, uint64_t morsel_size,
                                   uint64_t desc_len, uint32_t x_type,
                                   uint64_t x_len, uint32_t out_type,
                                   uint64_t out_m, uint32_t claim_n);
/* Validate a region's header against the mapping size and the binding's
   magic, including section placement. Returns NULL and fills *out (when
   non-NULL) on success, else a static message. Runs once per mapping. */
REI_API const char *rei_morsel_hdr_check(const void *base, size_t size,
                                         uint32_t magic,
                                         rei_morsel_hdr *out);

/* Batch-sizing state: process-private, never wire state, reset at each
   run's first-call CLAIM CAS. The binding embeds one per map context; a
   doorbell help that claims a queued runner of the same map aliases it (the
   cost is a mis-sized batch or a re-ramp on resume — harmless). */
typedef struct rei_morsel_sizer_s {
  int32_t  run_r;          /* ordinal whose ramp this is (-1 = none) */
  uint32_t run_gen;
  uint64_t k;              /* current batch size, morsels */
  uint64_t k_last;         /* morsels issued last transition */
  double   t_last;         /* rei_now() at the last issue */
  double   cost;           /* est. seconds per morsel (0 = unknown) */
  int      skip;           /* last interval contained a help: no update */
} rei_morsel_sizer;
REI_API void rei_morsel_sizer_init(rei_morsel_sizer *);

/* One whole batch transition — generation-fenced lane claim, cancel and
   pool-signal checks, sized cursor issue — over the mapped region. Returns
   1 and fills m (the 0-based first morsel), k (morsel count after the final
   partial grant) and help (the doorbell flag); 0 means stop: the lane was
   lost to the trim or a reset re-armed it, the cancel word fired, the pool
   is stopping, the owner died, or the cursor is exhausted. sig (may be
   NULL) is the worker-local pool-signal trio; pin_k != 0 fixes k (a test
   entry, bypassing the sizing policy); now is the caller's rei_now() read.
   Atomics only, no park: safe to call under a global lock. */
REI_API int rei_morsel_next(void *base, const rei_morsel_hdr *h,
                            rei_morsel_sizer *sz, uint32_t r, uint32_t gen,
                            const rei_pool_sig *sig, uint64_t pin_k,
                            double now, uint64_t *m_out, uint64_t *k_out,
                            int *help_out);

/* Prepared-run re-arm, O(1) in n (no per-morsel state exists to clear):
   bump the generation, stamp (new_gen << 2) | IDLE over the CLAIM array,
   zero the cursor, clear the cancel word. The stamped generation is the
   fence against a stale trimmed runner from the prior run: its first-call
   CAS expects the old generation and fails against the re-armed word
   however the reset interleaves. Returns the new generation — the value the
   next run's runner payloads must carry. */
REI_API uint32_t rei_morsel_reset(void *base, const rei_morsel_hdr *h);

/* The exhausted-runner trim's CAS, folding its own trigger: a no-op unless
   the cursor is exhausted or the cancel word is set. Returns the
   morsel-state verdict: REI_MORSEL_ABANDONED (won, or already trimmed),
   REI_MORSEL_RUNNING (executing or published — collect it),
   REI_MORSEL_IDLE (trigger unarmed: defer rather than park). */
REI_API int rei_morsel_abandon(void *base, const rei_morsel_hdr *h,
                               uint32_t r, uint32_t gen);

/* The cancel word: set by the submitter on timeout / interrupt / death, and
   by an erroring runner itself before its ERR publish — the fail-fast
   store that stops every peer within ~a batch. Idempotent. */
REI_API void rei_morsel_cancel_set(void *base, const rei_morsel_hdr *h);
REI_API int rei_morsel_cancel_get(const void *base, const rei_morsel_hdr *h);
/* Single reads of the mutable words: the generation (masked), the cursor
   clamped to n_morsels, and one CLAIM word (the protocol tests' view). */
REI_API uint32_t rei_morsel_generation(const void *base,
                                       const rei_morsel_hdr *h);
REI_API uint64_t rei_morsel_cursor(const void *base,
                                   const rei_morsel_hdr *h);
REI_API uint32_t rei_morsel_claim(const void *base, const rei_morsel_hdr *h,
                                  uint32_t r);

/* A batch's element range, 0-based half-open: [m * morsel_size, min((m + k)
   * morsel_size, n)). Bindings with 1-based inclusive ranges add 1 to lo. */
REI_API void rei_morsel_span_of(const rei_morsel_hdr *h, uint64_t m,
                                uint64_t k, uint64_t *lo, uint64_t *hi);

/* Worker-death lost set: issued = [0, bound), lost = issued minus the union
   of the collected batch histories — a batch in no history was issued but
   never completed (its claimant died, or the map fn errored mid-batch); a
   dead runner's whole history lands here too — it publishes only at
   exhaustion. spans (caller-assembled from the histories, element space,
   lo <= hi) is sorted in place; the gaps (at most n + 1) land in out as
   0-based half-open ranges; returns the gap count. */
typedef struct rei_morsel_span_s { uint64_t lo, hi; } rei_morsel_span;
REI_API size_t rei_morsel_lost(rei_morsel_span *spans, size_t n,
                               uint64_t bound, rei_morsel_span *out);

// The result sink and the unwind path (pool.c) ----------------------------------

/* The result-sink struct: the core fills it from a claimed entry and
   hands it to exec_fn; the binding passes it back to the publish verbs.
   payload/inline_max expose the slot's frame buffer so the binding
   frames its ERR envelope inline. */
struct rei_result_sink_s {
  rei_pool     *p;
  rei_rs_hdr   *rs;
  unsigned char *payload;   /* rs + sizeof(rei_rs_hdr): the frame buffer */
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
REI_API void rei_pool_eval_mark(rei_pool *, int in_flight);
/* The unwind path for a binding whose catching = 0 exec abandons (R's
   longjmp): when the eval marker says a task eval was in flight, clears
   it and mints that task's sink (from the identity the core saved at
   execute) for the err publish. Returns 1 then; 0 when the abandonment
   came from outside any task eval — infrastructure failure, the worker
   goes down. */
REI_API int rei_pool_unwind_sink(rei_pool *, rei_result_sink *out);

// Zero-copy refcount words ---------------------------------------------------------

/* The zc refcount / flags words of a REI* region header (bytes [24-31]
   of the reserved band — the wire-format offsets REI_ZC_REFCOUNT_OFF /
   REI_ZC_FLAGS_OFF in rei.h). Shared by the core release machinery and
   the bindings' view wrap/resolve paths. Dual form (see the banner):
   the exported symbols in src/ext.c are these same bodies. */
#ifdef REI_EXT_NO_INLINES
REI_API REI_ATOMIC(uint32_t) *rei_zc_rc(void *base);
REI_API REI_ATOMIC(uint32_t) *rei_zc_flags_(void *base);
#else
REI_EXT_INLINE REI_ATOMIC(uint32_t) *rei_zc_rc(void *base) {
  return (REI_ATOMIC(uint32_t) *) ((unsigned char *) base +
                                   REI_ZC_REFCOUNT_OFF);
}
REI_EXT_INLINE REI_ATOMIC(uint32_t) *rei_zc_flags_(void *base) {
  return (REI_ATOMIC(uint32_t) *) ((unsigned char *) base +
                                   REI_ZC_FLAGS_OFF);
}
#endif

// Wire-format helpers --------------------------------------------------------------

/* The small pure helpers over rei.h's wire-format conventions, dual form
   like the zc accessors above (see the banner): the timeout unit
   conversion, the NA_real_ store (for a binding filling converted output
   without R headers), the aux packing of the pool RAWSPILL and SHM_VEC
   kinds, and the flat REIH header write/check. */

#ifdef REI_EXT_NO_INLINES
REI_API double rei_timeout_ms(double seconds);
REI_API void rei_store_na_real(void *dst);
REI_API uint64_t rei_aux_rawspill_pool(int type, uint32_t name_len);
REI_API uint64_t rei_aux_shm_vec(int type, uint64_t total);
REI_API void rei_reih_write(void *base, int wire_type, int64_t n_elems);
REI_API int rei_reih_check(const void *base, size_t size,
                           int *wire_type, int64_t *n_elems);
#else
/* seconds (a binding's convention; <= 0 polls, non-finite waits
   indefinitely) to the core's timeout_ms (0 polls, < 0 indefinite). */
REI_EXT_INLINE double rei_timeout_ms(double seconds) {
  if (!isfinite(seconds)) return -1;
  return seconds <= 0 ? 0 : seconds * 1000;
}
REI_EXT_INLINE void rei_store_na_real(void *dst) {
  const uint64_t bits = REI_NA_REAL_BITS;
  memcpy(dst, &bits, 8);
}
/* aux = wire type tag | region name length << 8. */
REI_EXT_INLINE uint64_t rei_aux_rawspill_pool(int type, uint32_t name_len) {
  return (uint64_t) (uint32_t) type | ((uint64_t) name_len << 8);
}
/* aux = layout type tag | exact used bytes << 8. */
REI_EXT_INLINE uint64_t rei_aux_shm_vec(int type, uint64_t total) {
  return (uint64_t) (uint32_t) type | (total << 8);
}
/* The flat REIH header: magic, type, element count, zero attrs, and the
   reserved band [24, 64) zeroed. Write BEFORE rei_stage_retain_zc: the
   zeroing covers the zc refcount word (a recycled region carries a stale
   count), and the retain stores the producer loan after. */
REI_EXT_INLINE void rei_reih_write(void *base, int wire_type,
                                   int64_t n_elems) {
  const uint32_t magic = REI_MAGIC_VEC;
  const int32_t t32 = wire_type;
  const int64_t zero64 = 0;
  memcpy(base, &magic, 4);
  memcpy((unsigned char *) base + 4, &t32, 4);
  memcpy((unsigned char *) base + 8, &n_elems, 8);
  memcpy((unsigned char *) base + 16, &zero64, 8);
  memset((unsigned char *) base + 24, 0, REI_HEADER_SIZE - 24);
}
/* Validate the REIH header at base: region size, magic, a known atomic
   wire type, and the element/attribute extents against the region size —
   0 on success, -1 on any rejection. Attribute policy (whether the word
   at [16, 24) may be nonzero, and what it means) stays binding-side. */
REI_EXT_INLINE int rei_reih_check(const void *base, size_t size,
                                  int *wire_type, int64_t *n_elems) {
  if (size < REI_HEADER_SIZE) return -1;
  uint32_t magic;
  int32_t t32;
  int64_t len, attrs;
  memcpy(&magic, base, 4);
  if (magic != REI_MAGIC_VEC) return -1;
  memcpy(&t32, (const unsigned char *) base + 4, 4);
  memcpy(&len, (const unsigned char *) base + 8, 8);
  memcpy(&attrs, (const unsigned char *) base + 16, 8);
  const size_t elt = rei_type_elt_size(t32);
  if (elt == 0 || len < 0 || attrs < 0 ||
      len > ((int64_t) size - (int64_t) REI_HEADER_SIZE) / (int64_t) elt ||
      attrs > (int64_t) size - (int64_t) REI_HEADER_SIZE -
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
REI_API double rei_now(void);
REI_API long rei_self_pid(void);

/* Category + remediation text for an REI_ERRCAT (what the handle /
   thread-local error slot records). */
REI_API void rei_err_describe(rei_errcat, const char **summary,
                              const char **hint);

/* One 2^127-step L'Ecuyer-CMRG stream jump in place over a 6-word state
   (rng_jump.c; a binding's map derives per-element streams with it). */
REI_API void rei_rng_jump(int *seed);

/* malloc tuning (tune.c), called by the binding at load. */
REI_API void rei_tune(void);

#ifdef __cplusplus
}
#endif

#endif /* REI_EXT_H */
