/** \file mizu.h
    \brief libmizu public API: SPSC channels and work-stealing task pools
           between processes on one machine.

   libmizu is a C11 shared-memory IPC library: SPSC channels and a
   work-stealing task pool between processes on one machine. 64-bit only
   (the wire formats depend on lock-free 64-bit atomics); Linux requires
   kernel >= 5.3 (pidfd_open, no fallback).

   Three API tiers (the CPython PEP 689 model):
   - mizu.h (this header): the stable consumer API. The soname
     (libmizu.so.N = MIZU_VERSION_MAJOR) tracks its ABI; opaque handles +
     size-field structs keep it stable. The stable promise starts at
     1.0; until then any line may still move.
   - mizu_ext.h (installed alongside): the binding-author API — the
     callback seam, stager/read/publish services, and promoted
     internals. Version-pinned per minor release, may change without
     deprecation, ever. Language bindings compile against it (the
     first-party ones vendor the sources at a pinned commit).
   - src/internal.h: private, never installed.

   Two versioned contracts:
   - MIZU_ABI_VERSION: the wire format (shared-memory layouts). Peers
     validate it at attach; bump on any wire-format change. The
     wire-format structs below are this contract: preamble-versioned,
     not soname-frozen.
   - The library soname (above): the API/ABI of this header only.

   Conventions:
   - Handles are opaque typedefs; struct definitions live in
     src/internal.h. Nothing public passes or returns a struct by value;
     extensible structs carry a size field for ABI evolution.
   - Every public operation is a real exported function (MIZU_API). Macros
     and `static inline` are internal-only — an FFI cannot reach them.
   - Functions return mizu_status; destructors return void. Terminal
     transport states (FULL/TIMEOUT/CLOSED/PEER_GONE) are values, not
     errors; MIZU_ERR comes with a category + message (see Errors).
   - Timeouts are milliseconds as a double: < 0 indefinite, 0 polls.
     (Bindings convert from their own units.)
   - Callback threading: exec runs on the worker's own thread;
     death-watch callbacks run on OS listener threads; stage/read/drop
     and the check/park hooks run on the verb-calling thread.
     Documented per-function below.

   Not here: the binding seam (mizu_binding, the mizu_stage_* services,
   result publish, the bytes binding) and the promoted internals
   (parker, liveness lock, death watch, preamble, map support,
   mizu_now, the RNG jump kernel) are binding-author surface in
   mizu_ext.h; the handle struct definitions, spin machinery, and
   spill/ledger/open-cache internals stay private in src/internal.h. */

#ifndef MIZU_MIZU_H
#define MIZU_MIZU_H

#include <stddef.h>
#include <stdint.h>

/** The wire-format structs carry atomic words. C++ consumers get
   std::atomic, layout-compatible with _Atomic on every supported
   platform; only the C TUs rely on the atomic semantics. */
#ifdef __cplusplus
#  include <atomic>
#  define MIZU_ATOMIC(T) std::atomic<T>
#  define MIZU_STATIC_ASSERT static_assert
extern "C" {
#else
#  include <stdatomic.h>
#  define MIZU_ATOMIC(T) _Atomic T
#  define MIZU_STATIC_ASSERT _Static_assert
#endif

/** Export macro. MIZU_API defaults to empty: static linkage — vendored
   and amalgamated builds, the first-party packages — needs no macro.
   Shared-library builds define MIZU_SHARED, plus MIZU_BUILDING when
   compiling the library itself on Windows. */
#if defined(MIZU_SHARED)
#  if defined(_WIN32)
#    if defined(MIZU_BUILDING)
#      define MIZU_API __declspec(dllexport)
#    else
#      define MIZU_API __declspec(dllimport)
#    endif
#  else
#    define MIZU_API __attribute__((visibility("default")))
#  endif
#else
#  define MIZU_API
#endif

#define MIZU_VERSION_MAJOR 0
#define MIZU_VERSION_MINOR 0
#define MIZU_VERSION_PATCH 1

/** Library version, "major.minor.patch". Static storage; never freed. */
MIZU_API const char *mizu_version(void);

// Status and errors ------------------------------------------------------------

/** The verb result. MIZU_FULL: ring/slot capacity exhausted (try-send).
   MIZU_TIMEOUT: deadline passed. MIZU_CLOSED: orderly close. MIZU_PEER_GONE:
   the peer's liveness lock is released (the death verdict; listeners are
   wake triggers only). MIZU_ERR: a real error — category + message. */
typedef enum mizu_status_e {
  MIZU_OK = 0,
  MIZU_FULL,
  MIZU_TIMEOUT,
  MIZU_CLOSED,
  MIZU_PEER_GONE,
  MIZU_ERR
} mizu_status;

/** Portable failure categories for MIZU_ERR (the platform layer classifies
   errno / GetLastError). */
typedef enum mizu_errcat_e {
  MIZU_ERRCAT_NONE = 0,
  MIZU_ERRCAT_NOSPACE,      /**< ENOSPC / ERROR_DISK_FULL */
  MIZU_ERRCAT_NOMEMORY,     /**< ENOMEM / commit limit exceeded */
  MIZU_ERRCAT_EXISTS,       /**< region name already in use (orphan) */
  MIZU_ERRCAT_EXHAUSTED,    /**< pool result slots exhausted (submit) */
  MIZU_ERRCAT_STOPPED,      /**< pool stopped, or its owner died (submit) */
  MIZU_ERRCAT_STAGE,        /**< the binding's stage_fn returned nonzero */
  MIZU_ERRCAT_INTERRUPTED,  /**< the binding's check hook returned nonzero */
  MIZU_ERRCAT_OTHER
} mizu_errcat;

/** Error access. Handle verbs record the category + message on the handle;
   both are valid until the next call on that handle. Handle-free entry
   points (regions, prune) use a thread-local slot instead. */
MIZU_API mizu_errcat mizu_last_error_category(void);
MIZU_API const char *mizu_last_error_message(void);

// Opaque handles ---------------------------------------------------------------

typedef struct mizu_channel_s mizu_channel;
typedef struct mizu_pool_s mizu_pool;
typedef struct mizu_shm_s mizu_shm;
/** The binding callbacks struct, taken by pointer at create/attach/join.
   Binding-author surface: the definition, mizu_binding_init, and the
   built-in bytes binding live in mizu_ext.h. */
typedef struct mizu_binding_s mizu_binding;

// Wire format: channel region ----------------------------------------------------

/** First 64-byte line of every channel region. Host-written before spawn,
   immutable thereafter; validated by the peer before any shared atomic is
   read or written. */
#define MIZU_MAGIC        0x4D495A43u   /**< "MIZC" */
#define MIZU_ABI_VERSION  1u

typedef struct mizu_preamble_s {
  uint32_t magic;
  uint32_t version;          /**< MIZU_ABI_VERSION */
  uint32_t cap;              /**< slots per ring, power of two */
  uint32_t slot;             /**< slot size in bytes, power of two */
  uint64_t host_pid;         /**< the peer death listener's watch target */
  uint64_t arena_size;       /**< bytes per direction, multiple of 64, 0 disables */
  uint64_t drop_offset;      /**< peer bootstrap bytes, exact-sized (opaque */
  uint64_t drop_size;        /**<   to the core; tagged — see MIZU_DROP_*) */
  uint64_t livedir_offset;   /**< directory holding the two liveness files */
  uint64_t livedir_size;
} mizu_preamble;

MIZU_STATIC_ASSERT(sizeof(mizu_preamble) == 64, "mizu_preamble is the wire format");

/** The drop's first byte is a wire-format tag, so a foreign child can
   dispatch on it: MIZU_DROP_R — a native serialize stream (the
   homogeneous format); MIZU_DROP_SOURCE — UTF-8 source text in the peer's language,
   the cross-language lingua franca. The core treats the drop as opaque
   bytes; the tag is a binding convention. */
#define MIZU_DROP_R       0x52u   /**< 'R' */
#define MIZU_DROP_SOURCE  0x53u   /**< 'S' */

/** Fixed channel layout: preamble (0), rendezvous line (64), one entity
   block per side (128 host, 192 peer), then the four ring index lines
   from 256 — one full cache line per shared index. */
#define MIZU_ENTITY_HOST  0
#define MIZU_ENTITY_PEER  1
#define MIZU_ENTITY_OFFSET(i)  ((size_t) 128 + 64 * (size_t) (i))
#define MIZU_FIXED_LAYOUT_SIZE ((size_t) 512)

#define MIZU_OFF_READY      ((size_t) 64)
#define MIZU_OFF_CLOSED     ((size_t) 68)
#define MIZU_OFF_PEER_PID   ((size_t) 72)
#define MIZU_OFF_FLAGS      ((size_t) 80)
#define MIZU_FLAG_SPIN      1u   /**< pure-spin waiting: consumers never park */

#define MIZU_ENTITY_EPOCH   0
#define MIZU_ENTITY_PARKED  4
#define MIZU_ENTITY_REG     8
/** Offset 16 of the 64-byte entity block: the side's identity word
   (MIZU_IDENT in mizu_ext.h — language byte, reader-capability mask).
   The host writes it at create, before the token exists; the peer at
   attach, before ready_set (whose release store orders it for the host's
   ready_wait acquire). Immutable thereafter. */
#define MIZU_ENTITY_IDENT   16

#define MIZU_OFF_HP_TAIL    ((size_t) 256)
#define MIZU_OFF_HP_HEAD    ((size_t) 320)
#define MIZU_OFF_PH_TAIL    ((size_t) 384)
#define MIZU_OFF_PH_HEAD    ((size_t) 448)

// Wire format: payload framing -------------------------------------------------

/** A 16-byte header then payload bytes, shared by channel slots and pool
   entries / result slots. The aux/payload conventions per kind are wire
   contract (cross-language peers read them), not binding choice:
   - INLINE: a complete serialized stream (len = stream length; aux bit 0
     is MIZU_AUX_F_KEEPERLESS, the stager's keeperless claim; all other
     aux bits zero).
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
   - SHM_VEC: name of a region holding an MIZU* layout object (len = name
     length, payload = name; aux = layout type tag | exact used bytes
     << 8) — the consumer wraps a zero-copy view.
   - REF: the /mizu_ identifier of an object already in shm (payload =
     identifier; aux = 0).
   - NIL: immediate empty value — no bytes move.
   - STR1: a length-1 string (bytes in the payload, aux the encoding;
     MIZU_STR1_NA marks the missing string). */
typedef enum mizu_kind_e {
  MIZU_KIND_INLINE = 0,
  MIZU_KIND_ARENA,
  MIZU_KIND_SHM_RAW,
  MIZU_KIND_RAWVEC,
  MIZU_KIND_SHM_VEC,
  MIZU_KIND_REF,
  MIZU_KIND_NIL,
  MIZU_KIND_STR1,
  MIZU_KIND_RAWSPILL
} mizu_kind;

/** Bit 0 of the INLINE aux word: the stager's claim that staging this
   frame committed no retain-table entry, so a pool collect need not wake
   the producer's keeper sweep. Claim-only — the core reads but never
   verifies it, and cores predating the flag never read INLINE aux at
   all; clear is always correct (at worst a spurious keeper-sweep wake).
   Only INLINE carries the claim: every other kind's aux is fully
   assigned (see the kind docs above). */
#define MIZU_AUX_F_KEEPERLESS UINT64_C(1)

#define MIZU_STR1_NA UINT64_MAX   /**< cannot alias an encoding (0-3) */

/** The string-encoding byte's values, carried by STR1's aux and the MIZS
   block's per-string encoding section. The numbering is R's cetype_t,
   hoisted to the wire contract (bindings pin it with a static assert). An
   NA string's encoding byte is 0 and its span empty; the validity bit,
   not the byte, marks it. */
#define MIZU_CE_NATIVE 0u
#define MIZU_CE_UTF8   1u
#define MIZU_CE_LATIN1 2u
#define MIZU_CE_BYTES  3u

typedef struct mizu_slot_hdr_s {
  uint32_t kind;             /**< mizu_kind */
  uint32_t len;
  uint64_t aux;
} mizu_slot_hdr;

MIZU_STATIC_ASSERT(sizeof(mizu_slot_hdr) == 16, "mizu_slot_hdr is the wire format");

/** Wire type tags, fixed by the wire format (RAWVEC aux, MIZU* layout
   headers). Bindings map their own element types onto these. The numbers
   are libmizu-owned and frozen from the first release: they descend from
   R's SEXPTYPE space by history, never by dependency — a future SEXPTYPE
   joins the wire only by an explicit allocation here, as INT64 = 32
   already did. */
typedef enum mizu_type_e {
  MIZU_TYPE_LGL = 10,         /**< int32 logical; INT_MIN is the missing sentinel */
  MIZU_TYPE_INT = 13,         /**< int32; INT_MIN is the missing sentinel */
  MIZU_TYPE_REAL = 14,        /**< float64; a specific NaN payload is the sentinel */
  MIZU_TYPE_CPLX = 15,        /**< interleaved float64 re/im */
  MIZU_TYPE_STR = 16,         /**< string vector (MIZS layout) */
  MIZU_TYPE_VEC = 19,         /**< list tree (MIZL layout) */
  MIZU_TYPE_RAW = 24,         /**< bytes */
  MIZU_TYPE_INT64 = 32        /**< int64; INT64_MIN is the missing sentinel */
} mizu_type;

/** The missing-value sentinels of the atomic wire types (the R ABI fixes
   the bit patterns; a binding of any language writes them without R
   headers). Little-endian throughout, as the whole wire format is. */
#define MIZU_NA_INT32     INT32_MIN              /**< NA_integer_ / NA logical */
#define MIZU_NA_INT64     INT64_MIN              /**< NA_integer64_ sentinel */
#define MIZU_NA_REAL_BITS 0x7FF80000000007A2ULL  /**< NA_real_ (a NaN payload) */

/** Element size of an atomic wire type, 0 for non-atomic. */
MIZU_API size_t mizu_type_elt_size(int type);

/** Zero-copy view protocol: mizu-owned bytes [24-31] of every MIZU* region
   header (reserved [24-63] in the layout). [24-27] view refcount,
   [28-31] flags (bit 0 REFHELD: the identifier escaped by reference, so
   the death backstop leaks + unlinks instead of force-reclaiming). The
   producer stores 1 at stage; the consumer adds 1 at wrap before its
   consumer-done signal; view release subs 1. */
#define MIZU_ZC_REFCOUNT_OFF ((size_t) 24)
#define MIZU_ZC_FLAGS_OFF    ((size_t) 28)
#define MIZU_ZC_FLAG_REFHELD 1u

// Wire format: MIZH / MIZS / MIZL region layouts ---------------------------------

/** Region magics (first 4 bytes): atomic vector, string vector, list tree.
   Every layout opens with a 64-byte header; bytes [24-63] were reserved
   (written zero) — the zc protocol owns [24-31], and the two words below
   are now assigned: the format flags word and the validity section.

   Header bytes [32-35] are the format flags word. Bit 0 is the S4 object
   bit, which the layouts otherwise cannot carry; every other bit is
   reserved zero. This is a format word, not a negotiation word: a reader
   rejects a set bit it does not know as a corrupt or newer region (the
   identity word's reserved bits are the opposite — ignored). */
#define MIZU_HDR_FLAGS_OFF ((size_t) 32)
#define MIZU_HDR_FLAG_S4   1u

/** Header bytes [40-47] and [48-55] are the optional validity-bitmap
   section of every MIZH and MIZL header (nested MIZL included; MIZS
   carries its bitmap in the string block instead): an i64 offset and an
   i64 null count, three states. {0, 0}: absent — a pre-section region,
   the core's flat reserve, or a same-language write; the reader's lazy
   fallback. {0, -1}: known-NA-free (no section materialized). Else the
   offset of a 64-byte-aligned section: for MIZH a ceil(n / 8)-byte
   LSB-first bitmap (1 = present, the MIZS string block's convention);
   for MIZL a table of n {i64 offset, i64 null count} leaf entries, one
   per directory entry — a VECSXP or STRSXP entry's pair is {0, 0} (the
   nested header / the string block carries its own), and [48-55] is the
   total null count across that header's remaining leaves. Reserved-zero
   means absent, so a pre-dating reader simply never looks; the section
   needs no capability bit. */
#define MIZU_HDR_VALID_OFF   ((size_t) 40)
#define MIZU_HDR_VALID_COUNT ((size_t) 48)

/** MIZH (atomic vector): [4-7] i32 wire type, [8-15] i64 element count,
   [16-23] i64 attrs blob size; bare element bytes at 64, the attrs blob
   trailing. MIZS (string vector): [4-7] i32 attrs blob size, [8-15] i64
   string count, [16-23] i64 string-block byte size; the string block at
   64, the attrs blob trailing it. The string block (also the form of an
   MIZL STRSXP leaf): a ceil(n / 8)-byte validity bitmap, (n + 1) i64
   offsets, n encoding bytes (MIZU_CE_*), and the packed string bytes —
   each section 64-byte aligned from the block start. The first, second
   and fourth are Arrow large_utf8's three buffers verbatim; the encoding
   section is R's per-CHARSXP mark. MIZL (list tree): [4-7] i32 element
   count n, [8-15] i64 attrs blob offset, [16-23] i64 attrs blob size;
   then n 32-byte directory entries, each { i64 data_offset (64-byte
   aligned), i64 data_size, i32 sexptype, i32 attrs_size, i64 length }.
   The entry's data is the bare element bytes (an atomic leaf), a string
   block (STRSXP), a nested MIZL (VECSXP) or a serialized stream
   (sexptype 0); the leaf's attrs blob is the last attrs_size bytes of
   data_size. Bit 30 of the entry's sexptype is the leaf's S4 bit; the
   remainder is a listed tag — 0 (serialized), the atomic tags, STR, VEC,
   and 32 (MIZU_TYPE_INT64 is legal on MIZL leaves). Tag 33 is a remote
   leaf: the column lives in another region and crosses by reference.
   data_offset / data_size hold the identifier span — the region name,
   optionally name[i,j,...] with 1-based decimal indices, 1-255 bytes —
   length and attrs_size describe the referenced column as resolved, the
   S4 bit is never set, and the validity pair is a {0,0} / {0,-1} claim
   alone (DESIGN.md's remote-leaf rules are normative). */
#define MIZU_MIZL_S4 0x40000000

#define MIZU_MAGIC_VEC   0x4D495A48u  /**< "MIZH" */
#define MIZU_MAGIC_STR   0x4D495A53u  /**< "MIZS" */
#define MIZU_MAGIC_LIST  0x4D495A4Cu  /**< "MIZL" */
#define MIZU_HEADER_SIZE 64

#define MIZU_NAME_MAX 30  /**< fits Windows worst case + NUL; Darwin PSHMNAMLEN */

// Wire format: pool region -------------------------------------------------------

#define MIZU_POOL_MAGIC  0x4D495A50u  /**< "MIZP" */
/** Parked-worker bitmask width; also the map CLAIM array bound. */
#define MIZU_MAX_WORKERS 64

typedef struct mizu_pool_hdr_s {
  uint32_t magic;
  uint32_t version;
  uint32_t max_workers;      /**< <= MIZU_MAX_WORKERS */
  uint32_t max_submitters;   /**< <= 64: inj_ready_sub / full_waiters bits */
  uint32_t inj_cap;          /**< entries per submitter ring, power of two */
  uint32_t deque_cap;        /**< entries per worker deque, power of two */
  uint32_t result_slots;     /**< total; a multiple of max_submitters */
  uint32_t slot;             /**< bytes per entry / result slot, power of two */
  uint64_t owner_pid;        /**< the workers' death-listener watch target */
  uint64_t livedir_offset;
  uint64_t livedir_size;
  /** The workers' identity word (MIZU_IDENT in mizu_ext.h): 0 until the
     first worker join CASes its binding's word in; a join whose word
     differs fails, an exact match proceeds. Set for the pool's lifetime —
     never reset, so homogeneity is per pool, not per worker generation.
     The one field written after create: read it through the mapping,
     never the validated handle copy, which is stale the moment it lands. */
  MIZU_ATOMIC(uint64_t) worker_ident;
} mizu_pool_hdr;

MIZU_STATIC_ASSERT(sizeof(mizu_pool_hdr) == 64, "mizu_pool_hdr is the wire format");

/** Worker registry slot: two cache lines — admin + owner-written fields on
   line 0, thief-CAS'd deque_top apart on line 1. in_flight_rs/_seq are
   announced before any claim and read only post-mortem by a reaper
   serialized by the liveness lock. */
typedef struct mizu_wk_slot_s {
  MIZU_ATOMIC(int32_t)  status;        /**< MIZU_WK_* */
  int32_t          id;
  int64_t          pid;           /**< informational; never a liveness signal */
  MIZU_ATOMIC(int32_t)  park_state;    /**< MIZU_WPK_* */
  MIZU_ATOMIC(uint32_t) park_epoch;
  int64_t          deque_buf_off;
  int32_t          deque_cap;
  MIZU_ATOMIC(int32_t)  in_flight_rs;
  MIZU_ATOMIC(uint64_t) in_flight_seq;
  MIZU_ATOMIC(int64_t)  deque_bottom;
  MIZU_ATOMIC(int32_t)  retire;
  uint8_t          pad0[4];
  MIZU_ATOMIC(int64_t)  deque_top;
  uint64_t         live_dev;      /**< liveness-file identity, written once */
  uint64_t         live_ino;      /**<  at join before LIVE */
  MIZU_ATOMIC(uint64_t) stat_tasks;
  MIZU_ATOMIC(uint64_t) stat_steals;
  MIZU_ATOMIC(uint64_t) stat_inj;
  MIZU_ATOMIC(uint64_t) stat_parks;
  MIZU_ATOMIC(uint64_t) stat_helps;
} mizu_wk_slot;

MIZU_STATIC_ASSERT(sizeof(mizu_wk_slot) == 128, "mizu_wk_slot is the wire format");

typedef struct mizu_sub_slot_s {
  MIZU_ATOMIC(int32_t)  status;        /**< MIZU_SUB_* */
  MIZU_ATOMIC(uint32_t) park_epoch;
  int64_t          pid;
  uint32_t         rs_start;      /**< static partition: result_slots / */
  uint32_t         rs_count;      /**<  max_submitters */
  uint64_t         live_dev;
  uint64_t         live_ino;
  MIZU_ATOMIC(uint64_t) stat_spills;
  MIZU_ATOMIC(uint64_t) stat_spill_reuse;
  uint8_t          pad[8];
} mizu_sub_slot;

MIZU_STATIC_ASSERT(sizeof(mizu_sub_slot) == 64, "mizu_sub_slot is the wire format");

/** Result slot header; the payload framing header sits at offset 24 and
   payload bytes at 40. */
typedef struct mizu_rs_hdr_s {
  MIZU_ATOMIC(int32_t)  status;        /**< MIZU_RS_* */
  MIZU_ATOMIC(int32_t)  waiter_slot;   /**< submitter slot parked on this (-1) */
  MIZU_ATOMIC(uint64_t) sequence;      /**< increments on reuse: stale-handle check */
  MIZU_ATOMIC(int32_t)  worker_slot;   /**< executing worker (-1 until claimed) */
  uint32_t         pad;
  mizu_slot_hdr     ph;
} mizu_rs_hdr;

MIZU_STATIC_ASSERT(sizeof(mizu_rs_hdr) == 40, "mizu_rs_hdr is the wire format");

/** Injection ring / deque entry header; payload framing at offset 16,
   payload bytes at 32. flags bit 0 is MIZU_ENTRY_RUNNER; bits 1-2 are
   reserved for a future language tag (homogeneous pools in v1). */
typedef struct mizu_entry_hdr_s {
  uint64_t task_id;           /**< submitter slot << 48 | counter; debug only */
  uint32_t rs_index;
  uint16_t submitter_slot;
  uint16_t flags;
  mizu_slot_hdr ph;
} mizu_entry_hdr;

MIZU_STATIC_ASSERT(sizeof(mizu_entry_hdr) == 32, "mizu_entry_hdr is the wire format");

#define MIZU_ENTRY_RUNNER 1u

typedef enum mizu_wk_status_e { MIZU_WK_FREE = 0, MIZU_WK_CLAIMING, MIZU_WK_LIVE,
                               MIZU_WK_LEAVING, MIZU_WK_REAPING } mizu_wk_status;
typedef enum mizu_sub_status_e { MIZU_SUB_FREE = 0, MIZU_SUB_LIVE,
                                MIZU_SUB_REAPING } mizu_sub_status;
/** DIED is the reaper's terminal: status-word only, no payload — a reap
   cannot write payload bytes without racing a live worker's concurrent
   publish. The "worker died" error object is built collect-side, keyed
   off the status. */
typedef enum mizu_rs_status_e { MIZU_RS_FREE = 0, MIZU_RS_PENDING, MIZU_RS_OK,
                               MIZU_RS_ERR, MIZU_RS_CANCEL,
                               MIZU_RS_DIED } mizu_rs_status;
typedef enum mizu_park_state_e { MIZU_WPK_RUNNING = 0, MIZU_WPK_IDLE,
                                MIZU_WPK_PARKED, MIZU_WPK_WAKING } mizu_park_state;

/** Injection tier metadata (128 B) and control block (192 B) offsets:
   unrelated hot words, one cache line each. */
#define MIZU_INJ_META_SIZE     ((size_t) 128)
#define MIZU_INJ_TAIL_OFF      ((size_t) 0)
#define MIZU_INJ_HEAD_OFF      ((size_t) 64)
#define MIZU_TIER_READY_OFF    ((size_t) 0)
#define MIZU_TIER_FULL_OFF     ((size_t) 64)
#define MIZU_CTRL_SHUTDOWN_OFF ((size_t) 0)
#define MIZU_CTRL_PARKED_OFF   ((size_t) 64)
#define MIZU_CTRL_HELP_OFF     ((size_t) 128)

// Regions (mizu_shm) ----------------------------------------------------------------

/** Heap-only (the stack form stays internal). create pre-faults on
   Linux. open maps read-only; open_rw maps writable (populate
   pre-faults); open_view is the zc consumer open: page 0 RW (the
   refcount word), the rest read-only — and performs the zc counted add
   itself, so a binding cannot hold a view mapping without the count.
   open_view_flags is the flags form: MIZU_OPEN_VIEW_NOCOUNT skips the
   counted add — the caller then owns the mizu_zc_ref timing and must
   complete it before its consumer-done signal (a binding whose wrap can
   fail between map and count opens first and counts at wrap).
   close unmaps and frees the handle; unlink != 0 also removes the name.
   On failure these return MIZU_ERR with the category in the thread-local
   error slot. addr/size/name are borrowed reads, valid until close. */
#define MIZU_OPEN_VIEW_NOCOUNT 1u /**< caller performs the counted add itself */
MIZU_API mizu_status mizu_shm_create(mizu_shm **out, size_t size);
MIZU_API mizu_status mizu_shm_open(mizu_shm **out, const char *name);
MIZU_API mizu_status mizu_shm_open_rw(mizu_shm **out, const char *name,
                                   int populate);
MIZU_API mizu_status mizu_shm_open_view(mizu_shm **out, const char *name);
MIZU_API mizu_status mizu_shm_open_view_flags(mizu_shm **out, const char *name,
                                           uint32_t flags);
MIZU_API void mizu_shm_close(mizu_shm *, int unlink);
MIZU_API void *mizu_shm_addr(mizu_shm *);
MIZU_API size_t mizu_shm_size(const mizu_shm *);
MIZU_API const char *mizu_shm_name(const mizu_shm *);

/** Reap /mizu_ orphans of dead creators. Returns a malloc'd array of
   malloc'd names (free each, then the array), *n the count; NULL when
   none — including on platforms that cannot enumerate the shm namespace
   (Windows, where a mapping cannot outlive its creator). */
MIZU_API char **mizu_shm_reap(int *n);

/** The consumer side of the zc refcount protocol (header bytes [24-31] of
   a view-opened region): ref at view wrap, before consumer-done (already
   performed by mizu_shm_open_view; exported for bindings that map view
   pages themselves); unref once-only at view release. flag_refheld marks
   a region whose identifier is escaping by reference (REF emit). */
MIZU_API void mizu_zc_ref(mizu_shm *);
MIZU_API void mizu_zc_unref(mizu_shm *);
MIZU_API uint32_t mizu_zc_refcount(const mizu_shm *);
MIZU_API uint32_t mizu_zc_flags(const mizu_shm *);
MIZU_API void mizu_zc_flag_refheld(mizu_shm *);

// Channel verbs --------------------------------------------------------------------

/** size-stamped options; mizu_channel_opts_init zeroes + stamps. drop is
   the peer bootstrap payload, copied into the region at create (opaque
   bytes; its first byte is a MIZU_DROP_* tag by binding convention; the
   peer's binding reads them after attach). */
typedef struct mizu_channel_opts_s {
  uint32_t size;
  uint32_t capacity;      /**< ring slots, power of two, 2..2^24 */
  uint32_t slot_size;     /**< bytes per slot, power of two, 64..2^20 */
  uint64_t arena_size;    /**< bytes per direction; 0 disables */
  uint32_t flags;         /**< MIZU_FLAG_SPIN */
  const uint8_t *drop;    /**< peer bootstrap bytes (may be NULL) */
  uint64_t drop_size;
} mizu_channel_opts;

#define MIZU_CHANNEL_SPIN MIZU_FLAG_SPIN

MIZU_API void mizu_channel_opts_init(mizu_channel_opts *);

/** create: host side; writes the preamble, returns the handle. token
   writes the join token ("<pid hex>_<counter hex>") for the peer's
   attach; returns MIZU_ERR if cap is too small. attach: peer side;
   validates the preamble (magic + MIZU_ABI_VERSION) before any shared
   atomic is touched. The peer signals ready_set after attach; the host's
   ready_wait returns MIZU_OK/MIZU_TIMEOUT. */
MIZU_API mizu_status mizu_channel_create(mizu_channel **out,
                                      const mizu_channel_opts *,
                                      const mizu_binding *);
MIZU_API mizu_status mizu_channel_token(const mizu_channel *,
                                     char *buf, size_t cap);
MIZU_API mizu_status mizu_channel_attach(mizu_channel **out, const char *token,
                                      const mizu_binding *);
/** The peer bootstrap payload staged at create (the preamble's drop):
   borrowed bytes, valid until destroy. The peer's binding must consume
   them (e.g. unserialize the expression) before ready_set — the host holds
   everything they reference alive exactly until ready. */
MIZU_API void mizu_channel_drop(const mizu_channel *, const uint8_t **bytes,
                              uint64_t *n);
MIZU_API mizu_status mizu_channel_ready_set(mizu_channel *);
MIZU_API mizu_status mizu_channel_ready_wait(mizu_channel *, double timeout_ms);

/** send stages obj via the binding's stage_fn: MIZU_OK, MIZU_FULL (ring
   full — sends never block for ring space; retry or drop is the
   caller's), MIZU_CLOSED, MIZU_PEER_GONE. send_batch moves up to n objects
   in one batched tail store and sets *accepted_out — short of n when the
   ring filled or the channel closed midway (still MIZU_OK; send the next
   element singly to learn which).
   recv hands back the read_fn's product in *obj_out: MIZU_OK, MIZU_TIMEOUT,
   MIZU_CLOSED, MIZU_PEER_GONE. A terminal state is reported only once the
   ring is drained — the published messages of a closed or dead peer are
   complete and valid — and MIZU_PEER_GONE is sticky once returned.
   recv_batch waits for the first message exactly like recv (*n_out is 0
   on a terminal status), then drains up to cap already-published
   messages without waiting further. A batch returns every message it
   consumed: a read failure on the first message returns MIZU_ERR as
   recv does, but a failure after it ends the batch early with MIZU_OK
   and the messages read so far. The failing slot stays at the head, so
   the next receive reproduces the failure exactly (and consumes the
   slot if the binding marks it MIZU_READ_CONSUME). MIZU_READ_CONSUME
   is therefore honoured on a single receive and on a batch's first
   message only; past that the consume decision defers to the next
   receive.
   Callbacks run on the calling thread; the wait parks on the caller's
   entity. */
MIZU_API mizu_status mizu_channel_send(mizu_channel *, void *obj);
MIZU_API mizu_status mizu_channel_send_batch(mizu_channel *, void **objs,
                                          size_t n, size_t *accepted_out);
MIZU_API mizu_status mizu_channel_recv(mizu_channel *, void **obj_out,
                                    double timeout_ms);
MIZU_API mizu_status mizu_channel_recv_batch(mizu_channel *, void **objs,
                                          size_t cap, size_t *n_out,
                                          double timeout_ms);

/** The sink-callback form of recv_batch: each message is handed to sink
   as it is read, so a binding can anchor every object (a GC protect, a
   refcount) before the next read allocates — the array form forces a
   binding to hold n unanchored products across the remaining reads.
   Same wait and status discipline as recv_batch (which is a thin
   adapter over this). */
typedef void (*mizu_obj_sink)(void *ctx, size_t i, void *obj);
MIZU_API mizu_status mizu_channel_recv_batch_fn(mizu_channel *, size_t cap,
                                             size_t *n_out, mizu_obj_sink,
                                             void *ctx, double timeout_ms);

/** close signals this side's close bit, then waits up to timeout_ms for
   the peer's close bit or its death — the rendezvous that makes it safe
   to release the sent-payload pins, since the peer sets its bit only
   after draining. MIZU_OK: rendezvoused, resources released, the region
   unlinked, and the handle dead (destroy is a no-op after). MIZU_TIMEOUT:
   the handle stays usable and the rendezvous is retried at destroy.
   Idempotent. close_signal just sets this side's bit (the peer half of
   the protocol). After either side signals, sends on both sides return
   MIZU_CLOSED and receives drain before reporting it.
   alive is the peer-liveness verdict (lock probe, not the listener
   flag); a peer that closed but still runs reads as alive.
   destroy is the GC-finalizer target: idempotent, never blocks; on a
   handle that never rendezvoused it signals close and releases what the
   (non-blocking) rendezvous check completes — the death verdict and
   mizu_shm_reap backstop the rest. */
MIZU_API mizu_status mizu_channel_close(mizu_channel *, double timeout_ms);
MIZU_API mizu_status mizu_channel_close_signal(mizu_channel *);
/** Logically a read; the probe can run survivor cleanup internally. */
MIZU_API int mizu_channel_alive(const mizu_channel *);
MIZU_API void mizu_channel_destroy(mizu_channel *);

/** Handle-local + wire-state snapshot. name is borrowed. Counters are
   process-local where noted. */
typedef struct mizu_channel_info_s {
  uint32_t size;
  const char *name;
  int32_t side;             /**< MIZU_ENTITY_HOST / MIZU_ENTITY_PEER */
  uint32_t capacity, slot_size;
  uint64_t arena_size, inline_max;
  int32_t spin, ready, closed;
  int64_t peer_pid;
  uint64_t tx_sent, tx_published, tx_consumed, rx_consumed, rx_published;
  uint64_t fl_entries, fl_hits;                 /**< process-local */
  uint64_t open_hits, open_misses;              /**< process-local */
  uint64_t ledger_entries;                      /**< process-local */
  uint64_t zc_open_hits, zc_open_misses;        /**< binding-owned view cache;
   the core fills 0 */
} mizu_channel_info;

MIZU_API mizu_status mizu_channel_info_get(const mizu_channel *,
                                        mizu_channel_info *out);

/** Handle error access (MIZU_ERR results). Borrowed; valid until the next
   call on this handle. */
MIZU_API mizu_errcat mizu_channel_errcat(const mizu_channel *);
MIZU_API const char *mizu_channel_error(const mizu_channel *);

// Pool verbs ---------------------------------------------------------------------

typedef struct mizu_pool_opts_s {
  uint32_t size;
  uint32_t max_workers;     /**< <= MIZU_MAX_WORKERS */
  uint32_t max_submitters;  /**< <= 64 */
  uint32_t injection_cap;   /**< entries per submitter ring, power of two */
  uint32_t per_worker_cap;  /**< deque entries per worker, power of two */
  uint32_t result_slots;    /**< multiple of max_submitters */
  uint32_t slot_size;       /**< bytes per entry / result slot, pow2, 128..2^20 */
} mizu_pool_opts;

MIZU_API void mizu_pool_opts_init(mizu_pool_opts *);

/** A task handle: an 8-byte value identifying one result slot at one
   sequence — 40-bit sequence in the low bits, 24-bit result-slot index
   in the high (result_slots is capped at 2^24 at create). Core-filled
   at submit; bindings store and pass back by pointer. The 8-byte size
   lets a binding pack the whole handle into an external-pointer address
   (zero heap traffic per task). All-zero is the invalid value. */
typedef struct mizu_task_s {
  uint64_t word;
} mizu_task;

#define MIZU_TASK_SEQ_BITS 40
#define MIZU_TASK_SEQ_MAX ((UINT64_C(1) << MIZU_TASK_SEQ_BITS) - 1)

static inline uint64_t mizu_task_seq(const mizu_task *t) {
  return t->word & MIZU_TASK_SEQ_MAX;
}
static inline uint32_t mizu_task_rs_index(const mizu_task *t) {
  return (uint32_t) (t->word >> MIZU_TASK_SEQ_BITS);
}
static inline mizu_task mizu_task_make(uint64_t seq, uint32_t rs_index) {
  mizu_task t;
  t.word = (seq & MIZU_TASK_SEQ_MAX) |
           ((uint64_t) rs_index << MIZU_TASK_SEQ_BITS);
  return t;
}

/** create: controller side. token as the channel's. ready_wait covers the
   given worker slots (startup handshake). destroy releases the handle. */
MIZU_API mizu_status mizu_pool_create(mizu_pool **out, const mizu_pool_opts *,
                                   const mizu_binding *);
MIZU_API mizu_status mizu_pool_token(const mizu_pool *, char *buf, size_t cap);
MIZU_API mizu_status mizu_pool_ready_wait(mizu_pool *, const uint32_t *slots,
                                       size_t n, double timeout_ms);
MIZU_API void mizu_pool_destroy(mizu_pool *);

/** Submitter side: attach joins a live pool from another process,
   claiming a free submitter slot with its own injection ring and
   result-slot subrange. */
MIZU_API mizu_status mizu_pool_attach(mizu_pool **out, const char *token,
                                   const mizu_binding *);

/** Worker side: join attaches to token as worker `slot` (liveness lock
   before the status CAS; the binding's exec_fn is registered at
   join). run is the worker loop shell —
   claims/steals tasks and invokes exec_fn on this thread — blocking
   until it returns an exit reason. A nonzero exec_fn return is an
   infrastructure failure and takes the worker down (MIZU_EXIT_ERROR); a
   task's own error never reaches run — exec_fn flattens and publishes
   it through the sink. A retired worker (MIZU_EXIT_RETIRED) lingers as
   the lifetime anchor for its uncollected results: the binding loops
   lame_duck on a plain sleep (no unpark can reach a released slot)
   until it returns nonzero — shutdown or owner death ends the linger.
   leave is the clean-exit handshake: an announced in-flight claim (an
   exec_fn escape left unpublished) fails as DIED there, so an orderly
   leave after an infrastructure failure never strands a task — the
   collector's worker-death verdict, no reaper required. */
typedef enum mizu_worker_exit_e { MIZU_EXIT_SHUTDOWN = 0,
                                 MIZU_EXIT_OWNER_GONE,
                                 MIZU_EXIT_RETIRED,
                                 MIZU_EXIT_ERROR } mizu_worker_exit;

MIZU_API mizu_status mizu_pool_worker_join(mizu_pool **out, const char *token,
                                        uint32_t slot, const mizu_binding *);
MIZU_API mizu_worker_exit mizu_pool_worker_run(mizu_pool *);
MIZU_API int mizu_pool_lame_duck(mizu_pool *);
MIZU_API mizu_status mizu_pool_leave(mizu_pool *);

/** Controller only: ask one worker to exit cleanly — non-blocking, never
   preemptive; the worker observes between tasks and releases its slot,
   and the remaining workers consume its queued work in place. */
MIZU_API mizu_status mizu_pool_retire(mizu_pool *, uint32_t slot);

/** One claim at a time, for deterministic in-process harnesses (the
   pool_pair/pool_step discipline). Returns MIZU_STEP_TASK after a task,
   MIZU_STEP_IDLE on timeout, MIZU_STEP_SHUTDOWN on shutdown or owner
   death, MIZU_STEP_RETIRED on a retire request. An exec_fn infrastructure
   failure (its nonzero return) or an interrupt abandon records the error
   on the handle and returns MIZU_STEP_SHUTDOWN — the worker cannot
   continue; mizu_pool_errcat distinguishes it from a real shutdown. */
typedef enum mizu_step_result_e { MIZU_STEP_IDLE = 0, MIZU_STEP_TASK = 1,
                                 MIZU_STEP_SHUTDOWN = -1,
                                 MIZU_STEP_RETIRED = -2 } mizu_step_result;
MIZU_API int mizu_pool_step(mizu_pool *, double timeout_ms);

/** submit stages task_obj via the binding's stage_fn and fills *out.
   Submission blocks only on injection-ring space (back-pressure is
   per-submitter): MIZU_FULL when the ring is still full at the deadline
   (0 polls; a binding raises its submit-timeout error here). Result-slot
   exhaustion and a stopped pool / dead owner raise instead: MIZU_ERR
   with MIZU_ERRCAT_EXHAUSTED / MIZU_ERRCAT_STOPPED. On a worker handle a
   submit is nested: it pushes onto the worker's own deque (a full deque
   runs the task inline) and claims a submitter slot on first use.
   submit_batch applies the same per-task semantics, except that a ring
   still full at the deadline ends the batch early: MIZU_OK with
   *n_out < n (the single form's MIZU_FULL), the accepted handles valid
   and collectible. */
MIZU_API mizu_status mizu_pool_submit(mizu_pool *, void *task_obj,
                                   mizu_task *out, double timeout_ms);
MIZU_API mizu_status mizu_pool_submit_batch(mizu_pool *, void **objs, size_t n,
                                         mizu_task *out, size_t *n_out,
                                         double timeout_ms);

/** The supply-callback form of submit_batch: the binding produces task
   object i on demand, so it can stage through one reusable wire object
   instead of pre-building n of them. Same per-task semantics and wake
   cadence as the array form (which is a thin adapter over this). */
typedef void *(*mizu_obj_supply)(void *ctx, size_t i);
MIZU_API mizu_status mizu_pool_submit_batch_fn(mizu_pool *, mizu_obj_supply,
                                            void *ctx, size_t n,
                                            mizu_task *out, size_t *n_out,
                                            double timeout_ms);

/** collect waits for the task's terminal state and sets *value_out to
   the read_fn's product — including for non-OK outcomes, where
   ctx.outcome (MIZU_RS_ERR / MIZU_RS_CANCEL / MIZU_RS_DIED) lets the
   read_fn build the binding's error object (a binding re-raises it: the task's
   own condition, the cancellation, or the worker-death error).
   MIZU_TIMEOUT consumes nothing. A task is collected exactly once: the
   slot is released as the value is produced. Valid on worker handles
   too — a nested collect helps (executes/steals) instead of sleeping,
   so exec_fn reenters from inside collect; on submitter handles
   (exec == NULL) the wait simply parks. (A binding's collect may omit
   the pool argument: its task handle carries the pool reference.)
   collect_any returns the first terminal handle (*index_out, 0-based;
   ties among already-terminal handles break to the earliest position);
   the reported handle is consumed, the rest stay collectible.
   collect_all fills values_out in input order once every task is
   terminal. On the first non-OK outcome by position it reports that
   handle only: *err_index_out is its index, the reported handle is
   consumed (its error object rides read_fn like any value into
   values_out[err]), and every other handle — the OK results ahead of
   it included — stays collectible. *err_index_out == n means all OK.
   MIZU_TIMEOUT consumes nothing: every handle stays valid. */
MIZU_API mizu_status mizu_pool_collect(mizu_pool *, const mizu_task *,
                                    void **value_out, double timeout_ms);
MIZU_API mizu_status mizu_pool_collect_any(mizu_pool *, const mizu_task *,
                                        size_t n, size_t *index_out,
                                        void **value_out, double timeout_ms);
MIZU_API mizu_status mizu_pool_collect_all(mizu_pool *, const mizu_task *,
                                        size_t n, void **values_out,
                                        size_t *err_index_out,
                                        double timeout_ms);

/** The sink-callback form of collect_all: each value is handed to sink
   as it is claimed (input order when all OK; the first non-OK outcome
   alone otherwise), so a binding can anchor every object before the
   next claim's read allocates. Same wait and stop-at-error semantics
   as the array form (which is a thin adapter over this). */
MIZU_API mizu_status mizu_pool_collect_all_fn(mizu_pool *, const mizu_task *,
                                           size_t n, mizu_obj_sink,
                                           void *ctx,
                                           size_t *err_index_out,
                                           double timeout_ms);

/** Task lifecycle trace hook: per-handle, per-process; NULL removes.
   Worker-side events fire on the worker thread, MIZU_TRACE_SUBMIT on the
   calling thread. A hook error is an infrastructure failure at its site
   (on a worker, it takes the worker down) — unlike a task's own error,
   which is that task's ERR result. */
typedef enum mizu_trace_event_e { MIZU_TRACE_SUBMIT = 0, MIZU_TRACE_START,
                                 MIZU_TRACE_DONE, MIZU_TRACE_ERROR,
                                 MIZU_TRACE_DROP,
                                 MIZU_TRACE_REHOME } mizu_trace_event;
typedef void (*mizu_trace_fn)(mizu_trace_event event, uint64_t task_id,
                             void *ctx);
MIZU_API mizu_status mizu_pool_set_trace(mizu_pool *, mizu_trace_fn, void *ctx);

/** Advisory and discard-only, never preemptive: a still-queued task is
   skipped; an executing one runs to completion and its result is
   dropped. Returns 1 when this call cancelled the task, 0 when too
   late (completed, already cancelled, pool gone) — every edge folds
   into 0, there is no error path. A completed slot is left collectible;
   releasing a handle that will never be collected is
   mizu_pool_task_release. */
MIZU_API int mizu_pool_cancel(mizu_pool *, const mizu_task *);
/** The finalizer release for a task handle that was never collected: a
   still-pending task is cancelled (as mizu_pool_cancel); a completed
   (OK/ERR/DIED) slot is freed, letting the producing worker's keeper
   sweep drop what the result retained. Returns 1 only when this call
   cancelled a pending task. Total, no error path: a stale handle, a
   released pool, or a forked child answers 0. */
MIZU_API int mizu_pool_task_release(mizu_pool *, const mizu_task *);
/** MIZU_RS_* of a task (informational, racy against slot reuse). */
MIZU_API int mizu_pool_task_state(mizu_pool *, const mizu_task *);
/** Orderly shutdown, controller only: broadcasts shutdown, wakes every
   parked participant, and cancels all pending tasks (blocked collectors
   see MIZU_RS_CANCEL), then waits up to timeout_ms for clean worker
   exits and unlinks the region and liveness files. MIZU_TIMEOUT: the
   workers still exit on their own. The handle is dead afterwards;
   stopping again is a no-op. destroy on a controller handle broadcasts
   shutdown the same way, without the wait. */
MIZU_API mizu_status mizu_pool_stop(mizu_pool *, double timeout_ms);

/** Snapshots. status reads shm-resident wire state; dump adds
   handle-local counters (free list, open caches, collect parks) — the
   cold-path introspection API behind a binding's dump output.
   Per-slot arrays are indexed by slot; only the first n_workers /
   n_submitters entries are valid. The per-slot worker states are what a
   resize scans for free slots. All borrowed; valid until the next call. */
typedef struct mizu_pool_status_s {
  uint32_t size;
  const char *name;
  int32_t role;             /**< MIZU_ROLE_* */
  uint32_t max_workers, max_submitters, injection_cap, result_slots,
           slot_size;
  uint32_t n_workers, n_submitters;
  uint8_t worker_state[MIZU_MAX_WORKERS];   /**< MIZU_WK_* */
  uint8_t sub_state[64];                   /**< MIZU_SUB_* */
  int64_t deque_depth[MIZU_MAX_WORKERS];    /**< bottom - top */
  uint64_t parked_mask;                    /**< one bit per worker slot */
  uint32_t tasks_by_state[6];              /**< occupancy indexed by MIZU_RS_* */
  uint32_t inj_queued[64];                 /**< unclaimed injection entries */
  int32_t shutdown;
} mizu_pool_status;

typedef enum mizu_role_e { MIZU_ROLE_CONTROLLER = 0, MIZU_ROLE_WORKER,
                          MIZU_ROLE_SUBMITTER } mizu_role;

typedef struct mizu_worker_stat_s {
  int32_t status, park_state;
  int64_t pid;
  uint64_t tasks, steals, injections, parks, helps;
  int64_t deque_top, deque_bottom;
  int32_t in_flight_rs;
} mizu_worker_stat;

typedef struct mizu_sub_stat_s {
  int32_t status;                   /**< MIZU_SUB_* */
  int64_t pid;
  uint32_t rs_start, rs_count;      /**< the static result-slot partition */
  uint64_t injected, claimed;       /**< ring tail / head, monotonic from 0 */
  uint64_t spills, spill_reuse;     /**< SHM_RAW-class staging counters */
  int32_t ready;                    /**< its inj_ready bit */
  int32_t full_waiter;              /**< its full_waiters bit */
} mizu_sub_stat;

typedef struct mizu_pool_dump_s {
  uint32_t size;
  mizu_pool_status status;
  uint32_t n_workers;                 /**< <= MIZU_MAX_WORKERS */
  mizu_worker_stat workers[MIZU_MAX_WORKERS];
  uint32_t n_submitters;              /**< <= 64 */
  mizu_sub_stat submitters[64];
  uint64_t fl_entries, fl_bytes, fl_hits;       /**< handle-local */
  uint64_t open_hits, open_misses;              /**< handle-local */
  uint64_t collect_parks;                       /**< handle-local */
  uint64_t ledger_entries;                      /**< handle-local */
  int32_t help_wanted;
} mizu_pool_dump;

/** One non-FREE result-slot row (a dump's task table). */
typedef struct mizu_rs_row_s {
  uint32_t slot;
  int32_t status;                   /**< MIZU_RS_* */
  uint64_t sequence;
  int32_t worker_slot;
  int32_t waiter_slot;
} mizu_rs_row;

MIZU_API mizu_status mizu_pool_status_get(const mizu_pool *,
                                       mizu_pool_status *out);
MIZU_API mizu_status mizu_pool_dump_get(const mizu_pool *, mizu_pool_dump *out);
/** The dump's task table, paged by the caller: fills up to cap rows in
   slot order (non-FREE slots only) and sets *n_out to the total non-FREE
   count, so a short buffer is answered by a second call with a bigger
   one. States can move mid-fill — a cold-path snapshot, like dump. */
MIZU_API mizu_status mizu_pool_tasks_get(const mizu_pool *, mizu_rs_row *out,
                                      uint32_t cap, uint32_t *n_out);

MIZU_API mizu_errcat mizu_pool_errcat(const mizu_pool *);
MIZU_API const char *mizu_pool_error(const mizu_pool *);

#ifdef __cplusplus
}
#endif

#endif /**< MIZU_MIZU_H */
