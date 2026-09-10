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
   is C-only binding surface, so macros are fine, and exactly three hot
   accessors ship in dual form (below). Every other operation is a real
   exported function (REI_API), so FFI consumers see the full tier.

   Dual-form fast paths (the CPython Py_INCREF pattern):
   rei_parker_snapshot, rei_zc_rc, and rei_zc_flags_ ship as
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
   binding.ctx pointer registered at create/attach/join — the stage hook
   receives the handle but not the ctx, so a binding whose per-handle
   context lives off the core handle reaches it here. spill_info fills
   the producer free-list entry count and the lent-region ledger count
   (debug/dump surface; the per-kind public snapshots are
   rei_channel_info_get / rei_pool_dump_get). */
REI_API int rei_handle_kind(const rei_handle *);
REI_API int rei_handle_churn(const rei_handle *);
REI_API void *rei_handle_binding_ctx(const rei_handle *);
REI_API void rei_handle_spill_info(const rei_handle *,
                                   uint32_t *fl_entries,
                                   uint32_t *ledger_entries);

// The binding seam ---------------------------------------------------------------

/* The core never sees a language object. A binding registers these
   callbacks at create/attach/join; the core copies them into the handle,
   so a hot-path call is one load + a predicted indirect branch.

   stage: frame obj as (hdr, payload) — payload capacity inline_max.
     Spill/arena/retain via the rei_stage_* services on the handle.
     Returns 0 on success, nonzero on staging failure (the verb fails as
     REI_ERR / REI_ERRCAT_STAGE). May also not return (a binding's
     longjmp):
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
     propagates by returning NULL.
   exec: pool workers only — run one claimed task frame and publish
     through the sink. Must not abandon: the binding catches every task
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
                            rei_handle *);

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
  void   *reserved[4];  /* zero; future growth without a soname bump */
} rei_read_ctx;

typedef void *(*rei_read_fn)(const rei_slot_hdr *, const uint8_t *payload,
                             size_t limit, rei_read_ctx *);

typedef int (*rei_exec_fn)(const rei_slot_hdr *hdr, const uint8_t *payload,
                           size_t limit, rei_result_sink *, int catching,
                           void *ctx);
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
