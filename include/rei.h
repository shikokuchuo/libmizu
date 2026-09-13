/* rei.h — librei public API

   librei is a C11 shared-memory IPC library: SPSC channels and a
   work-stealing task pool between processes on one machine. 64-bit only
   (the wire formats depend on lock-free 64-bit atomics); Linux requires
   kernel >= 5.3 (pidfd_open, no fallback).

   Three API tiers (the CPython PEP 689 model):
   - rei.h (this header): the stable consumer API. The soname
     (librei.so.N = REI_VERSION_MAJOR) tracks its ABI; opaque handles +
     size-field structs keep it stable. The stable promise starts at
     1.0; until then any line may still move.
   - rei_ext.h (installed alongside): the binding-author API — the
     callback seam, stager/read/publish services, and promoted
     internals. Version-pinned per minor release, may change without
     deprecation, ever. Language bindings compile against it (the
     first-party ones vendor the sources at a pinned commit).
   - src/internal.h: private, never installed.

   Two versioned contracts:
   - REI_ABI_VERSION: the wire format (shared-memory layouts). Peers
     validate it at attach; bump on any wire-format change. The
     wire-format structs below are this contract: preamble-versioned,
     not soname-frozen.
   - The library soname (above): the API/ABI of this header only.

   Conventions:
   - Handles are opaque typedefs; struct definitions live in
     src/internal.h. Nothing public passes or returns a struct by value;
     extensible structs carry a size field for ABI evolution.
   - Every public operation is a real exported function (REI_API). Macros
     and `static inline` are internal-only — an FFI cannot reach them.
   - Functions return rei_status; destructors return void. Terminal
     transport states (FULL/TIMEOUT/CLOSED/PEER_GONE) are values, not
     errors; REI_ERR comes with a category + message (see Errors).
   - Timeouts are milliseconds as a double: < 0 indefinite, 0 polls.
     (Bindings convert from their own units.)
   - Callback threading: exec runs on the worker's own thread;
     death-watch callbacks run on OS listener threads; stage/read/drop
     and the check/park hooks run on the verb-calling thread.
     Documented per-function below.

   Not here: the binding seam (rei_binding, the rei_stage_* services,
   result publish, the bytes binding) and the promoted internals
   (parker, liveness lock, death watch, preamble, map support,
   rei_now, the RNG jump kernel) are binding-author surface in
   rei_ext.h; the handle struct definitions, spin machinery, and
   spill/ledger/open-cache internals stay private in src/internal.h. */

#ifndef REI_REI_H
#define REI_REI_H

#include <stddef.h>
#include <stdint.h>

/* The wire-format structs carry atomic words. C++ consumers get
   std::atomic, layout-compatible with _Atomic on every supported
   platform; only the C TUs rely on the atomic semantics. */
#ifdef __cplusplus
#  include <atomic>
#  define REI_ATOMIC(T) std::atomic<T>
#  define REI_STATIC_ASSERT static_assert
extern "C" {
#else
#  include <stdatomic.h>
#  define REI_ATOMIC(T) _Atomic T
#  define REI_STATIC_ASSERT _Static_assert
#endif

/* Export macro. REI_API defaults to empty: static linkage — vendored
   and amalgamated builds, the first-party packages — needs no macro.
   Shared-library builds define REI_SHARED, plus REI_BUILDING when
   compiling the library itself on Windows. */
#if defined(REI_SHARED)
#  if defined(_WIN32)
#    if defined(REI_BUILDING)
#      define REI_API __declspec(dllexport)
#    else
#      define REI_API __declspec(dllimport)
#    endif
#  else
#    define REI_API __attribute__((visibility("default")))
#  endif
#else
#  define REI_API
#endif

#define REI_VERSION_MAJOR 0
#define REI_VERSION_MINOR 0
#define REI_VERSION_PATCH 1

/* Library version, "major.minor.patch". Static storage; never freed. */
REI_API const char *rei_version(void);

// Status and errors ------------------------------------------------------------

/* The verb result. REI_FULL: ring/slot capacity exhausted (try-send).
   REI_TIMEOUT: deadline passed. REI_CLOSED: orderly close. REI_PEER_GONE:
   the peer's liveness lock is released (the death verdict; listeners are
   wake triggers only). REI_ERR: a real error — category + message. */
typedef enum rei_status_e {
  REI_OK = 0,
  REI_FULL,
  REI_TIMEOUT,
  REI_CLOSED,
  REI_PEER_GONE,
  REI_ERR
} rei_status;

/* Portable failure categories for REI_ERR (the platform layer classifies
   errno / GetLastError). */
typedef enum rei_errcat_e {
  REI_ERRCAT_NONE = 0,
  REI_ERRCAT_NOSPACE,      /* ENOSPC / ERROR_DISK_FULL */
  REI_ERRCAT_NOMEMORY,     /* ENOMEM / commit limit exceeded */
  REI_ERRCAT_EXISTS,       /* region name already in use (orphan) */
  REI_ERRCAT_EXHAUSTED,    /* pool result slots exhausted (submit) */
  REI_ERRCAT_STOPPED,      /* pool stopped, or its owner died (submit) */
  REI_ERRCAT_STAGE,        /* the binding's stage_fn returned nonzero */
  REI_ERRCAT_INTERRUPTED,  /* the binding's check hook returned nonzero */
  REI_ERRCAT_OTHER
} rei_errcat;

/* Error access. Handle verbs record the category + message on the handle;
   both are valid until the next call on that handle. Handle-free entry
   points (regions, prune) use a thread-local slot instead. */
REI_API rei_errcat rei_last_error_category(void);
REI_API const char *rei_last_error_message(void);

// Opaque handles ---------------------------------------------------------------

typedef struct rei_channel_s rei_channel;
typedef struct rei_pool_s rei_pool;
typedef struct rei_shm_s rei_shm;
/* The binding callbacks struct, taken by pointer at create/attach/join.
   Binding-author surface: the definition, rei_binding_init, and the
   built-in bytes binding live in rei_ext.h. */
typedef struct rei_binding_s rei_binding;

// Wire format: channel region ----------------------------------------------------

/* First 64-byte line of every channel region. Host-written before spawn,
   immutable thereafter; validated by the peer before any shared atomic is
   read or written. */
#define REI_MAGIC        0x52454943u   /* "REIC" */
#define REI_ABI_VERSION  1u

typedef struct rei_preamble_s {
  uint32_t magic;
  uint32_t version;          /* REI_ABI_VERSION */
  uint32_t cap;              /* slots per ring, power of two */
  uint32_t slot;             /* slot size in bytes, power of two */
  uint64_t host_pid;         /* the peer death listener's watch target */
  uint64_t arena_size;       /* bytes per direction, multiple of 64, 0 disables */
  uint64_t drop_offset;      /* peer bootstrap bytes, exact-sized (opaque */
  uint64_t drop_size;        /*   to the core; tagged — see REI_DROP_*) */
  uint64_t livedir_offset;   /* directory holding the two liveness files */
  uint64_t livedir_size;
} rei_preamble;

REI_STATIC_ASSERT(sizeof(rei_preamble) == 64, "rei_preamble is the wire format");

/* The drop's first byte is a wire-format tag, so a foreign child can
   dispatch on it: REI_DROP_R — a native serialize stream (the
   homogeneous format); REI_DROP_SOURCE — UTF-8 source text in the peer's language,
   the cross-language lingua franca. The core treats the drop as opaque
   bytes; the tag is a binding convention. */
#define REI_DROP_R       0x52u   /* 'R' */
#define REI_DROP_SOURCE  0x53u   /* 'S' */

/* Fixed channel layout: preamble (0), rendezvous line (64), one entity
   block per side (128 host, 192 peer), then the four ring index lines
   from 256 — one full cache line per shared index. */
#define REI_ENTITY_HOST  0
#define REI_ENTITY_PEER  1
#define REI_ENTITY_OFFSET(i)  ((size_t) 128 + 64 * (size_t) (i))
#define REI_FIXED_LAYOUT_SIZE ((size_t) 512)

#define REI_OFF_READY      ((size_t) 64)
#define REI_OFF_CLOSED     ((size_t) 68)
#define REI_OFF_PEER_PID   ((size_t) 72)
#define REI_OFF_FLAGS      ((size_t) 80)
#define REI_FLAG_SPIN      1u   /* pure-spin waiting: consumers never park */

#define REI_ENTITY_EPOCH   0
#define REI_ENTITY_PARKED  4
#define REI_ENTITY_REG     8

#define REI_OFF_HP_TAIL    ((size_t) 256)
#define REI_OFF_HP_HEAD    ((size_t) 320)
#define REI_OFF_PH_TAIL    ((size_t) 384)
#define REI_OFF_PH_HEAD    ((size_t) 448)

// Wire format: payload framing -------------------------------------------------

/* A 16-byte header then payload bytes, shared by channel slots and pool
   entries / result slots. The aux/payload conventions per kind are wire
   contract (cross-language peers read them), not binding choice:
   - INLINE: a complete serialized stream (len = stream length; aux = 0).
   - ARENA (channel-only): one chunk in the spill arena (aux = chunk
     offset, byte length as uint64 in the payload).
   - SHM_RAW: name of a region holding the stream (len = name length,
     payload = name; aux = exact stream length).
   - RAWVEC: bare bytes of an attribute-free atomic vector, slot-resident
     (len = byte count; aux = wire type tag).
   - RAWSPILL: RAWVEC out of line — len is the byte count and aux & 0xff
     the wire type tag in both framings. Channel: an arena chunk
     (aux = type, payload = uint64 offset). Pool: a spill region
     (aux = type | name length << 8, payload = name).
   - SHM_VEC: name of a region holding an REI* layout object (len = name
     length, payload = name; aux = layout type tag | exact used bytes
     << 8) — the consumer wraps a zero-copy view.
   - REF: the /rei_ identifier of an object already in shm (payload =
     identifier; aux = 0).
   - NIL: immediate empty value — no bytes move.
   - STR1: a length-1 string (bytes in the payload, aux the encoding;
     REI_STR1_NA marks the missing string). */
typedef enum rei_kind_e {
  REI_KIND_INLINE = 0,
  REI_KIND_ARENA,
  REI_KIND_SHM_RAW,
  REI_KIND_RAWVEC,
  REI_KIND_SHM_VEC,
  REI_KIND_REF,
  REI_KIND_NIL,
  REI_KIND_STR1,
  REI_KIND_RAWSPILL
} rei_kind;

#define REI_STR1_NA UINT64_MAX   /* cannot alias an encoding (0-3) */

typedef struct rei_slot_hdr_s {
  uint32_t kind;             /* rei_kind */
  uint32_t len;
  uint64_t aux;
} rei_slot_hdr;

REI_STATIC_ASSERT(sizeof(rei_slot_hdr) == 16, "rei_slot_hdr is the wire format");

/* Wire type tags, fixed by the wire format (RAWVEC aux, REI* layout
   headers). Bindings map their own element types onto these. */
typedef enum rei_type_e {
  REI_TYPE_LGL = 10,         /* int32 logical; INT_MIN is the missing sentinel */
  REI_TYPE_INT = 13,         /* int32; INT_MIN is the missing sentinel */
  REI_TYPE_REAL = 14,        /* float64; a specific NaN payload is the sentinel */
  REI_TYPE_CPLX = 15,        /* interleaved float64 re/im */
  REI_TYPE_STR = 16,         /* string vector (REIS layout) */
  REI_TYPE_VEC = 19,         /* list tree (REIL layout) */
  REI_TYPE_RAW = 24,         /* bytes */
  REI_TYPE_INT64 = 32        /* int64; INT64_MIN is the missing sentinel */
} rei_type;

/* The missing-value sentinels of the atomic wire types (the R ABI fixes
   the bit patterns; a binding of any language writes them without R
   headers). Little-endian throughout, as the whole wire format is. */
#define REI_NA_INT32     INT32_MIN              /* NA_integer_ / NA logical */
#define REI_NA_INT64     INT64_MIN              /* NA_integer64_ sentinel */
#define REI_NA_REAL_BITS 0x7FF80000000007A2ULL  /* NA_real_ (a NaN payload) */

/* Element size of an atomic wire type, 0 for non-atomic. */
REI_API size_t rei_type_elt_size(int type);

/* Zero-copy view protocol: rei-owned bytes [24-31] of every REI* region
   header (reserved [24-63] in the layout). [24-27] view refcount,
   [28-31] flags (bit 0 REFHELD: the identifier escaped by reference, so
   the death backstop leaks + unlinks instead of force-reclaiming). The
   producer stores 1 at stage; the consumer adds 1 at wrap before its
   consumer-done signal; view release subs 1. */
#define REI_ZC_REFCOUNT_OFF ((size_t) 24)
#define REI_ZC_FLAGS_OFF    ((size_t) 28)
#define REI_ZC_FLAG_REFHELD 1u

// Wire format: REIH / REIS / REIL region layouts ---------------------------------

/* Region magics (first 4 bytes): atomic vector, string vector, list tree.
   Every layout opens with a 64-byte header; bytes [24-63] are reserved
   (written zero) — the zc protocol owns [24-31]. */
#define REI_MAGIC_VEC   0x52454948u  /* "REIH" */
#define REI_MAGIC_STR   0x52454953u  /* "REIS" */
#define REI_MAGIC_LIST  0x5245494Cu  /* "REIL" */
#define REI_HEADER_SIZE 64

#define REI_NAME_MAX 30  /* fits Windows worst case + NUL; Darwin PSHMNAMLEN */

// Wire format: pool region -------------------------------------------------------

#define REI_POOL_MAGIC  0x52454950u  /* "REIP" */
/* Parked-worker bitmask width; also the map CLAIM array bound. */
#define REI_MAX_WORKERS 64

typedef struct rei_pool_hdr_s {
  uint32_t magic;
  uint32_t version;
  uint32_t max_workers;      /* <= REI_MAX_WORKERS */
  uint32_t max_submitters;   /* <= 64: inj_ready_sub / full_waiters bits */
  uint32_t inj_cap;          /* entries per submitter ring, power of two */
  uint32_t deque_cap;        /* entries per worker deque, power of two */
  uint32_t result_slots;     /* total; a multiple of max_submitters */
  uint32_t slot;             /* bytes per entry / result slot, power of two */
  uint64_t owner_pid;        /* the workers' death-listener watch target */
  uint64_t livedir_offset;
  uint64_t livedir_size;
  uint8_t  pad[8];
} rei_pool_hdr;

REI_STATIC_ASSERT(sizeof(rei_pool_hdr) == 64, "rei_pool_hdr is the wire format");

/* Worker registry slot: two cache lines — admin + owner-written fields on
   line 0, thief-CAS'd deque_top apart on line 1. in_flight_rs/_seq are
   announced before any claim and read only post-mortem by a reaper
   serialized by the liveness lock. */
typedef struct rei_wk_slot_s {
  REI_ATOMIC(int32_t)  status;        /* REI_WK_* */
  int32_t          id;
  int64_t          pid;           /* informational; never a liveness signal */
  REI_ATOMIC(int32_t)  park_state;    /* REI_WPK_* */
  REI_ATOMIC(uint32_t) park_epoch;
  int64_t          deque_buf_off;
  int32_t          deque_cap;
  REI_ATOMIC(int32_t)  in_flight_rs;
  REI_ATOMIC(uint64_t) in_flight_seq;
  REI_ATOMIC(int64_t)  deque_bottom;
  REI_ATOMIC(int32_t)  retire;
  uint8_t          pad0[4];
  REI_ATOMIC(int64_t)  deque_top;
  uint64_t         live_dev;      /* liveness-file identity, written once */
  uint64_t         live_ino;      /*  at join before LIVE */
  REI_ATOMIC(uint64_t) stat_tasks;
  REI_ATOMIC(uint64_t) stat_steals;
  REI_ATOMIC(uint64_t) stat_inj;
  REI_ATOMIC(uint64_t) stat_parks;
  REI_ATOMIC(uint64_t) stat_helps;
} rei_wk_slot;

REI_STATIC_ASSERT(sizeof(rei_wk_slot) == 128, "rei_wk_slot is the wire format");

typedef struct rei_sub_slot_s {
  REI_ATOMIC(int32_t)  status;        /* REI_SUB_* */
  REI_ATOMIC(uint32_t) park_epoch;
  int64_t          pid;
  uint32_t         rs_start;      /* static partition: result_slots / */
  uint32_t         rs_count;      /*  max_submitters */
  uint64_t         live_dev;
  uint64_t         live_ino;
  REI_ATOMIC(uint64_t) stat_spills;
  REI_ATOMIC(uint64_t) stat_spill_reuse;
  uint8_t          pad[8];
} rei_sub_slot;

REI_STATIC_ASSERT(sizeof(rei_sub_slot) == 64, "rei_sub_slot is the wire format");

/* Result slot header; the payload framing header sits at offset 24 and
   payload bytes at 40. */
typedef struct rei_rs_hdr_s {
  REI_ATOMIC(int32_t)  status;        /* REI_RS_* */
  REI_ATOMIC(int32_t)  waiter_slot;   /* submitter slot parked on this (-1) */
  REI_ATOMIC(uint64_t) sequence;      /* increments on reuse: stale-handle check */
  REI_ATOMIC(int32_t)  worker_slot;   /* executing worker (-1 until claimed) */
  uint32_t         pad;
  rei_slot_hdr     ph;
} rei_rs_hdr;

REI_STATIC_ASSERT(sizeof(rei_rs_hdr) == 40, "rei_rs_hdr is the wire format");

/* Injection ring / deque entry header; payload framing at offset 16,
   payload bytes at 32. flags bit 0 is REI_ENTRY_RUNNER; bits 1-2 are
   reserved for a future language tag (homogeneous pools in v1). */
typedef struct rei_entry_hdr_s {
  uint64_t task_id;           /* submitter slot << 48 | counter; debug only */
  uint32_t rs_index;
  uint16_t submitter_slot;
  uint16_t flags;
  rei_slot_hdr ph;
} rei_entry_hdr;

REI_STATIC_ASSERT(sizeof(rei_entry_hdr) == 32, "rei_entry_hdr is the wire format");

#define REI_ENTRY_RUNNER 1u

typedef enum rei_wk_status_e { REI_WK_FREE = 0, REI_WK_CLAIMING, REI_WK_LIVE,
                               REI_WK_LEAVING, REI_WK_REAPING } rei_wk_status;
typedef enum rei_sub_status_e { REI_SUB_FREE = 0, REI_SUB_LIVE,
                                REI_SUB_REAPING } rei_sub_status;
/* DIED is the reaper's terminal: status-word only, no payload — a reap
   cannot write payload bytes without racing a live worker's concurrent
   publish. The "worker died" error object is built collect-side, keyed
   off the status. */
typedef enum rei_rs_status_e { REI_RS_FREE = 0, REI_RS_PENDING, REI_RS_OK,
                               REI_RS_ERR, REI_RS_CANCEL,
                               REI_RS_DIED } rei_rs_status;
typedef enum rei_park_state_e { REI_WPK_RUNNING = 0, REI_WPK_IDLE,
                                REI_WPK_PARKED, REI_WPK_WAKING } rei_park_state;

/* Injection tier metadata (128 B) and control block (192 B) offsets:
   unrelated hot words, one cache line each. */
#define REI_INJ_META_SIZE     ((size_t) 128)
#define REI_INJ_TAIL_OFF      ((size_t) 0)
#define REI_INJ_HEAD_OFF      ((size_t) 64)
#define REI_TIER_READY_OFF    ((size_t) 0)
#define REI_TIER_FULL_OFF     ((size_t) 64)
#define REI_CTRL_SHUTDOWN_OFF ((size_t) 0)
#define REI_CTRL_PARKED_OFF   ((size_t) 64)
#define REI_CTRL_HELP_OFF     ((size_t) 128)

// Regions (rei_shm) ----------------------------------------------------------------

/* Heap-only (the stack form stays internal). create pre-faults on
   Linux. open maps read-only; open_rw maps writable (populate
   pre-faults); open_view is the zc consumer open: page 0 RW (the
   refcount word), the rest read-only — and performs the zc counted add
   itself, so a binding cannot hold a view mapping without the count.
   open_view_flags is the flags form: REI_OPEN_VIEW_NOCOUNT skips the
   counted add — the caller then owns the rei_zc_ref timing and must
   complete it before its consumer-done signal (a binding whose wrap can
   fail between map and count opens first and counts at wrap).
   close unmaps and frees the handle; unlink != 0 also removes the name.
   On failure these return REI_ERR with the category in the thread-local
   error slot. addr/size/name are borrowed reads, valid until close. */
#define REI_OPEN_VIEW_NOCOUNT 1u /* caller performs the counted add itself */
REI_API rei_status rei_shm_create(rei_shm **out, size_t size);
REI_API rei_status rei_shm_open(rei_shm **out, const char *name);
REI_API rei_status rei_shm_open_rw(rei_shm **out, const char *name,
                                   int populate);
REI_API rei_status rei_shm_open_view(rei_shm **out, const char *name);
REI_API rei_status rei_shm_open_view_flags(rei_shm **out, const char *name,
                                           uint32_t flags);
REI_API void rei_shm_close(rei_shm *, int unlink);
REI_API void *rei_shm_addr(rei_shm *);
REI_API size_t rei_shm_size(const rei_shm *);
REI_API const char *rei_shm_name(const rei_shm *);

/* Reap /rei_ orphans of dead creators. Returns a malloc'd array of
   malloc'd names (free each, then the array), *n the count; NULL when
   none — including on platforms that cannot enumerate the shm namespace
   (Windows, where a mapping cannot outlive its creator). */
REI_API char **rei_shm_reap(int *n);

/* The consumer side of the zc refcount protocol (header bytes [24-31] of
   a view-opened region): ref at view wrap, before consumer-done (already
   performed by rei_shm_open_view; exported for bindings that map view
   pages themselves); unref once-only at view release. flag_refheld marks
   a region whose identifier is escaping by reference (REF emit). */
REI_API void rei_zc_ref(rei_shm *);
REI_API void rei_zc_unref(rei_shm *);
REI_API uint32_t rei_zc_refcount(const rei_shm *);
REI_API uint32_t rei_zc_flags(const rei_shm *);
REI_API void rei_zc_flag_refheld(rei_shm *);

// Channel verbs --------------------------------------------------------------------

/* size-stamped options; rei_channel_opts_init zeroes + stamps. drop is
   the peer bootstrap payload, copied into the region at create (opaque
   bytes; its first byte is a REI_DROP_* tag by binding convention; the
   peer's binding reads them after attach). */
typedef struct rei_channel_opts_s {
  uint32_t size;
  uint32_t capacity;      /* ring slots, power of two, 2..2^24 */
  uint32_t slot_size;     /* bytes per slot, power of two, 64..2^20 */
  uint64_t arena_size;    /* bytes per direction; 0 disables */
  uint32_t flags;         /* REI_FLAG_SPIN */
  const uint8_t *drop;    /* peer bootstrap bytes (may be NULL) */
  uint64_t drop_size;
} rei_channel_opts;

#define REI_CHANNEL_SPIN REI_FLAG_SPIN

REI_API void rei_channel_opts_init(rei_channel_opts *);

/* create: host side; writes the preamble, returns the handle. token
   writes the join token ("<pid hex>_<counter hex>") for the peer's
   attach; returns REI_ERR if cap is too small. attach: peer side;
   validates the preamble (magic + REI_ABI_VERSION) before any shared
   atomic is touched. The peer signals ready_set after attach; the host's
   ready_wait returns REI_OK/REI_TIMEOUT. */
REI_API rei_status rei_channel_create(rei_channel **out,
                                      const rei_channel_opts *,
                                      const rei_binding *);
REI_API rei_status rei_channel_token(const rei_channel *,
                                     char *buf, size_t cap);
REI_API rei_status rei_channel_attach(rei_channel **out, const char *token,
                                      const rei_binding *);
/* The peer bootstrap payload staged at create (the preamble's drop):
   borrowed bytes, valid until destroy. The peer's binding must consume
   them (e.g. unserialize the expression) before ready_set — the host holds
   everything they reference alive exactly until ready. */
REI_API void rei_channel_drop(const rei_channel *, const uint8_t **bytes,
                              uint64_t *n);
REI_API rei_status rei_channel_ready_set(rei_channel *);
REI_API rei_status rei_channel_ready_wait(rei_channel *, double timeout_ms);

/* send stages obj via the binding's stage_fn: REI_OK, REI_FULL (ring
   full — sends never block for ring space; retry or drop is the
   caller's), REI_CLOSED, REI_PEER_GONE. send_batch moves up to n objects
   in one batched tail store and sets *accepted_out — short of n when the
   ring filled or the channel closed midway (still REI_OK; send the next
   element singly to learn which).
   recv hands back the read_fn's product in *obj_out: REI_OK, REI_TIMEOUT,
   REI_CLOSED, REI_PEER_GONE. A terminal state is reported only once the
   ring is drained — the published messages of a closed or dead peer are
   complete and valid — and REI_PEER_GONE is sticky once returned.
   recv_batch waits for the first message exactly like recv (*n_out is 0
   on a terminal status), then drains up to cap already-published
   messages without waiting further.
   Callbacks run on the calling thread; the wait parks on the caller's
   entity. */
REI_API rei_status rei_channel_send(rei_channel *, void *obj);
REI_API rei_status rei_channel_send_batch(rei_channel *, void **objs,
                                          size_t n, size_t *accepted_out);
REI_API rei_status rei_channel_recv(rei_channel *, void **obj_out,
                                    double timeout_ms);
REI_API rei_status rei_channel_recv_batch(rei_channel *, void **objs,
                                          size_t cap, size_t *n_out,
                                          double timeout_ms);

/* The sink-callback form of recv_batch: each message is handed to sink
   as it is read, so a binding can anchor every object (a GC protect, a
   refcount) before the next read allocates — the array form forces a
   binding to hold n unanchored products across the remaining reads.
   Same wait and status discipline as recv_batch (which is a thin
   adapter over this). */
typedef void (*rei_obj_sink)(void *ctx, size_t i, void *obj);
REI_API rei_status rei_channel_recv_batch_fn(rei_channel *, size_t cap,
                                             size_t *n_out, rei_obj_sink,
                                             void *ctx, double timeout_ms);

/* close signals this side's close bit, then waits up to timeout_ms for
   the peer's close bit or its death — the rendezvous that makes it safe
   to release the sent-payload pins, since the peer sets its bit only
   after draining. REI_OK: rendezvoused, resources released, the region
   unlinked, and the handle dead (destroy is a no-op after). REI_TIMEOUT:
   the handle stays usable and the rendezvous is retried at destroy.
   Idempotent. close_signal just sets this side's bit (the peer half of
   the protocol). After either side signals, sends on both sides return
   REI_CLOSED and receives drain before reporting it.
   alive is the peer-liveness verdict (lock probe, not the listener
   flag); a peer that closed but still runs reads as alive.
   destroy is the GC-finalizer target: idempotent, never blocks; on a
   handle that never rendezvoused it signals close and releases what the
   (non-blocking) rendezvous check completes — the death verdict and
   rei_shm_reap backstop the rest. */
REI_API rei_status rei_channel_close(rei_channel *, double timeout_ms);
REI_API rei_status rei_channel_close_signal(rei_channel *);
/* Logically a read; the probe can run survivor cleanup internally. */
REI_API int rei_channel_alive(const rei_channel *);
REI_API void rei_channel_destroy(rei_channel *);

/* Handle-local + wire-state snapshot. name is borrowed. Counters are
   process-local where noted. */
typedef struct rei_channel_info_s {
  uint32_t size;
  const char *name;
  int32_t side;             /* REI_ENTITY_HOST / REI_ENTITY_PEER */
  uint32_t capacity, slot_size;
  uint64_t arena_size, inline_max;
  int32_t spin, ready, closed;
  int64_t peer_pid;
  uint64_t tx_sent, tx_published, tx_consumed, rx_consumed, rx_published;
  uint64_t fl_entries, fl_hits;                 /* process-local */
  uint64_t open_hits, open_misses;              /* process-local */
  uint64_t ledger_entries;                      /* process-local */
  uint64_t zc_open_hits, zc_open_misses;        /* binding-owned view cache;
                                                   the core fills 0 */
} rei_channel_info;

REI_API rei_status rei_channel_info_get(const rei_channel *,
                                        rei_channel_info *out);

/* Handle error access (REI_ERR results). Borrowed; valid until the next
   call on this handle. */
REI_API rei_errcat rei_channel_errcat(const rei_channel *);
REI_API const char *rei_channel_error(const rei_channel *);

// Pool verbs ---------------------------------------------------------------------

typedef struct rei_pool_opts_s {
  uint32_t size;
  uint32_t max_workers;     /* <= REI_MAX_WORKERS */
  uint32_t max_submitters;  /* <= 64 */
  uint32_t injection_cap;   /* entries per submitter ring, power of two */
  uint32_t per_worker_cap;  /* deque entries per worker, power of two */
  uint32_t result_slots;    /* multiple of max_submitters */
  uint32_t slot_size;       /* bytes per entry / result slot, pow2, 128..2^20 */
} rei_pool_opts;

REI_API void rei_pool_opts_init(rei_pool_opts *);

/* A task handle: an 8-byte value identifying one result slot at one
   sequence — 40-bit sequence in the low bits, 24-bit result-slot index
   in the high (result_slots is capped at 2^24 at create). Core-filled
   at submit; bindings store and pass back by pointer. The 8-byte size
   lets a binding pack the whole handle into an external-pointer address
   (zero heap traffic per task). All-zero is the invalid value. */
typedef struct rei_task_s {
  uint64_t word;
} rei_task;

#define REI_TASK_SEQ_BITS 40
#define REI_TASK_SEQ_MAX ((UINT64_C(1) << REI_TASK_SEQ_BITS) - 1)

static inline uint64_t rei_task_seq(const rei_task *t) {
  return t->word & REI_TASK_SEQ_MAX;
}
static inline uint32_t rei_task_rs_index(const rei_task *t) {
  return (uint32_t) (t->word >> REI_TASK_SEQ_BITS);
}
static inline rei_task rei_task_make(uint64_t seq, uint32_t rs_index) {
  rei_task t;
  t.word = (seq & REI_TASK_SEQ_MAX) |
           ((uint64_t) rs_index << REI_TASK_SEQ_BITS);
  return t;
}

/* create: controller side. token as the channel's. ready_wait covers the
   given worker slots (startup handshake). destroy releases the handle. */
REI_API rei_status rei_pool_create(rei_pool **out, const rei_pool_opts *,
                                   const rei_binding *);
REI_API rei_status rei_pool_token(const rei_pool *, char *buf, size_t cap);
REI_API rei_status rei_pool_ready_wait(rei_pool *, const uint32_t *slots,
                                       size_t n, double timeout_ms);
REI_API void rei_pool_destroy(rei_pool *);

/* Submitter side: attach joins a live pool from another process,
   claiming a free submitter slot with its own injection ring and
   result-slot subrange. */
REI_API rei_status rei_pool_attach(rei_pool **out, const char *token,
                                   const rei_binding *);

/* Worker side: join attaches to token as worker `slot` (liveness lock
   before the status CAS; the binding's exec_fn is registered at
   join). run is the worker loop shell —
   claims/steals tasks and invokes exec_fn on this thread — blocking
   until it returns an exit reason. A nonzero exec_fn return is an
   infrastructure failure and takes the worker down (REI_EXIT_ERROR); a
   task's own error never reaches run — exec_fn flattens and publishes
   it through the sink. A retired worker (REI_EXIT_RETIRED) lingers as
   the lifetime anchor for its uncollected results: the binding loops
   lame_duck on a plain sleep (no unpark can reach a released slot)
   until it returns nonzero — shutdown or owner death ends the linger.
   leave is the clean-exit handshake. */
typedef enum rei_worker_exit_e { REI_EXIT_SHUTDOWN = 0,
                                 REI_EXIT_OWNER_GONE,
                                 REI_EXIT_RETIRED,
                                 REI_EXIT_ERROR } rei_worker_exit;

REI_API rei_status rei_pool_worker_join(rei_pool **out, const char *token,
                                        uint32_t slot, const rei_binding *);
REI_API rei_worker_exit rei_pool_worker_run(rei_pool *);
REI_API int rei_pool_lame_duck(rei_pool *);
REI_API rei_status rei_pool_leave(rei_pool *);

/* Controller only: ask one worker to exit cleanly — non-blocking, never
   preemptive; the worker observes between tasks and releases its slot,
   and the remaining workers consume its queued work in place. */
REI_API rei_status rei_pool_retire(rei_pool *, uint32_t slot);

/* One claim at a time, for deterministic in-process harnesses (the
   pool_pair/pool_step discipline). Returns REI_STEP_TASK after a task,
   REI_STEP_IDLE on timeout, REI_STEP_SHUTDOWN on shutdown or owner
   death, REI_STEP_RETIRED on a retire request. An exec_fn infrastructure
   failure (its nonzero return) or an interrupt abandon records the error
   on the handle and returns REI_STEP_SHUTDOWN — the worker cannot
   continue; rei_pool_errcat distinguishes it from a real shutdown. */
typedef enum rei_step_result_e { REI_STEP_IDLE = 0, REI_STEP_TASK = 1,
                                 REI_STEP_SHUTDOWN = -1,
                                 REI_STEP_RETIRED = -2 } rei_step_result;
REI_API int rei_pool_step(rei_pool *, double timeout_ms);

/* submit stages task_obj via the binding's stage_fn and fills *out.
   Submission blocks only on injection-ring space (back-pressure is
   per-submitter): REI_FULL when the ring is still full at the deadline
   (0 polls; a binding raises its submit-timeout error here). Result-slot
   exhaustion and a stopped pool / dead owner raise instead: REI_ERR
   with REI_ERRCAT_EXHAUSTED / REI_ERRCAT_STOPPED. On a worker handle a
   submit is nested: it pushes onto the worker's own deque (a full deque
   runs the task inline) and claims a submitter slot on first use.
   submit_batch applies the same per-task semantics, except that a ring
   still full at the deadline ends the batch early: REI_OK with
   *n_out < n (the single form's REI_FULL), the accepted handles valid
   and collectible. */
REI_API rei_status rei_pool_submit(rei_pool *, void *task_obj,
                                   rei_task *out, double timeout_ms);
REI_API rei_status rei_pool_submit_batch(rei_pool *, void **objs, size_t n,
                                         rei_task *out, size_t *n_out,
                                         double timeout_ms);

/* The supply-callback form of submit_batch: the binding produces task
   object i on demand, so it can stage through one reusable wire object
   instead of pre-building n of them. Same per-task semantics and wake
   cadence as the array form (which is a thin adapter over this). */
typedef void *(*rei_obj_supply)(void *ctx, size_t i);
REI_API rei_status rei_pool_submit_batch_fn(rei_pool *, rei_obj_supply,
                                            void *ctx, size_t n,
                                            rei_task *out, size_t *n_out,
                                            double timeout_ms);

/* collect waits for the task's terminal state and sets *value_out to
   the read_fn's product — including for non-OK outcomes, where
   ctx.outcome (REI_RS_ERR / REI_RS_CANCEL / REI_RS_DIED) lets the
   read_fn build the binding's error object (a binding re-raises it: the task's
   own condition, the cancellation, or the worker-death error).
   REI_TIMEOUT consumes nothing. A task is collected exactly once: the
   slot is released as the value is produced. Valid on worker handles
   too — a nested collect helps (executes/steals) instead of sleeping,
   so exec_fn reenters from inside collect; on submitter handles
   (exec == NULL) the wait simply parks. (A binding's collect may omit
   the pool argument: its task handle carries the pool reference.)
   collect_any returns the first terminal handle (*index_out, 0-based;
   ties among already-terminal handles break to the earliest position);
   the reported handle is consumed, the rest stay collectible.
   collect_all fills values_out in input order once every task is
   terminal. On the first non-OK outcome by position it stops there:
   *err_index_out is its index, values_out is filled through that index
   inclusive (the error object rides read_fn like any value), handles
   past it stay collectible, and *err_index_out == n means all OK.
   REI_TIMEOUT consumes nothing: every handle stays valid. */
REI_API rei_status rei_pool_collect(rei_pool *, const rei_task *,
                                    void **value_out, double timeout_ms);
REI_API rei_status rei_pool_collect_any(rei_pool *, const rei_task *,
                                        size_t n, size_t *index_out,
                                        void **value_out, double timeout_ms);
REI_API rei_status rei_pool_collect_all(rei_pool *, const rei_task *,
                                        size_t n, void **values_out,
                                        size_t *err_index_out,
                                        double timeout_ms);

/* The sink-callback form of collect_all: each value is handed to sink
   as it is claimed (input order, through the first non-OK outcome
   inclusive), so a binding can anchor every object before the next
   claim's read allocates. Same wait and stop-at-error semantics as the
   array form (which is a thin adapter over this). */
REI_API rei_status rei_pool_collect_all_fn(rei_pool *, const rei_task *,
                                           size_t n, rei_obj_sink,
                                           void *ctx,
                                           size_t *err_index_out,
                                           double timeout_ms);

/* Task lifecycle trace hook: per-handle, per-process; NULL removes.
   Worker-side events fire on the worker thread, REI_TRACE_SUBMIT on the
   calling thread. A hook error is an infrastructure failure at its site
   (on a worker, it takes the worker down) — unlike a task's own error,
   which is that task's ERR result. */
typedef enum rei_trace_event_e { REI_TRACE_SUBMIT = 0, REI_TRACE_START,
                                 REI_TRACE_DONE, REI_TRACE_ERROR,
                                 REI_TRACE_DROP,
                                 REI_TRACE_REHOME } rei_trace_event;
typedef void (*rei_trace_fn)(rei_trace_event event, uint64_t task_id,
                             void *ctx);
REI_API rei_status rei_pool_set_trace(rei_pool *, rei_trace_fn, void *ctx);

/* Advisory and discard-only, never preemptive: a still-queued task is
   skipped; an executing one runs to completion and its result is
   dropped. Returns 1 when this call cancelled the task, 0 when too
   late (completed, already cancelled, pool gone) — every edge folds
   into 0, there is no error path. A completed slot is left collectible;
   releasing a handle that will never be collected is
   rei_pool_task_release. */
REI_API int rei_pool_cancel(rei_pool *, const rei_task *);
/* The finalizer release for a task handle that was never collected: a
   still-pending task is cancelled (as rei_pool_cancel); a completed
   (OK/ERR/DIED) slot is freed, letting the producing worker's keeper
   sweep drop what the result retained. Returns 1 only when this call
   cancelled a pending task. Total, no error path: a stale handle, a
   released pool, or a forked child answers 0. */
REI_API int rei_pool_task_release(rei_pool *, const rei_task *);
/* REI_RS_* of a task (informational, racy against slot reuse). */
REI_API int rei_pool_task_state(rei_pool *, const rei_task *);
/* Orderly shutdown, controller only: broadcasts shutdown, wakes every
   parked participant, and cancels all pending tasks (blocked collectors
   see REI_RS_CANCEL), then waits up to timeout_ms for clean worker
   exits and unlinks the region and liveness files. REI_TIMEOUT: the
   workers still exit on their own. The handle is dead afterwards;
   stopping again is a no-op. destroy on a controller handle broadcasts
   shutdown the same way, without the wait. */
REI_API rei_status rei_pool_stop(rei_pool *, double timeout_ms);

/* Snapshots. status reads shm-resident wire state; dump adds
   handle-local counters (free list, open caches, collect parks) — the
   cold-path introspection API behind a binding's dump output.
   Per-slot arrays are indexed by slot; only the first n_workers /
   n_submitters entries are valid. The per-slot worker states are what a
   resize scans for free slots. All borrowed; valid until the next call. */
typedef struct rei_pool_status_s {
  uint32_t size;
  const char *name;
  int32_t role;             /* REI_ROLE_* */
  uint32_t max_workers, max_submitters, injection_cap, result_slots,
           slot_size;
  uint32_t n_workers, n_submitters;
  uint8_t worker_state[REI_MAX_WORKERS];   /* REI_WK_* */
  uint8_t sub_state[64];                   /* REI_SUB_* */
  int64_t deque_depth[REI_MAX_WORKERS];    /* bottom - top */
  uint64_t parked_mask;                    /* one bit per worker slot */
  uint32_t tasks_by_state[6];              /* occupancy indexed by REI_RS_* */
  uint32_t inj_queued[64];                 /* unclaimed injection entries */
  int32_t shutdown;
} rei_pool_status;

typedef enum rei_role_e { REI_ROLE_CONTROLLER = 0, REI_ROLE_WORKER,
                          REI_ROLE_SUBMITTER } rei_role;

typedef struct rei_worker_stat_s {
  int32_t status, park_state;
  int64_t pid;
  uint64_t tasks, steals, injections, parks, helps;
  int64_t deque_top, deque_bottom;
  int32_t in_flight_rs;
} rei_worker_stat;

typedef struct rei_sub_stat_s {
  int32_t status;                   /* REI_SUB_* */
  int64_t pid;
  uint32_t rs_start, rs_count;      /* the static result-slot partition */
  uint64_t injected, claimed;       /* ring tail / head, monotonic from 0 */
  uint64_t spills, spill_reuse;     /* SHM_RAW-class staging counters */
  int32_t ready;                    /* its inj_ready bit */
  int32_t full_waiter;              /* its full_waiters bit */
} rei_sub_stat;

typedef struct rei_pool_dump_s {
  uint32_t size;
  rei_pool_status status;
  uint32_t n_workers;                 /* <= REI_MAX_WORKERS */
  rei_worker_stat workers[REI_MAX_WORKERS];
  uint32_t n_submitters;              /* <= 64 */
  rei_sub_stat submitters[64];
  uint64_t fl_entries, fl_bytes, fl_hits;       /* handle-local */
  uint64_t open_hits, open_misses;              /* handle-local */
  uint64_t collect_parks;                       /* handle-local */
  uint64_t ledger_entries;                      /* handle-local */
  int32_t help_wanted;
} rei_pool_dump;

/* One non-FREE result-slot row (a dump's task table). */
typedef struct rei_rs_row_s {
  uint32_t slot;
  int32_t status;                   /* REI_RS_* */
  uint64_t sequence;
  int32_t worker_slot;
  int32_t waiter_slot;
} rei_rs_row;

REI_API rei_status rei_pool_status_get(const rei_pool *,
                                       rei_pool_status *out);
REI_API rei_status rei_pool_dump_get(const rei_pool *, rei_pool_dump *out);
/* The dump's task table, paged by the caller: fills up to cap rows in
   slot order (non-FREE slots only) and sets *n_out to the total non-FREE
   count, so a short buffer is answered by a second call with a bigger
   one. States can move mid-fill — a cold-path snapshot, like dump. */
REI_API rei_status rei_pool_tasks_get(const rei_pool *, rei_rs_row *out,
                                      uint32_t cap, uint32_t *n_out);

REI_API rei_errcat rei_pool_errcat(const rei_pool *);
REI_API const char *rei_pool_error(const rei_pool *);

#ifdef __cplusplus
}
#endif

#endif /* REI_REI_H */
