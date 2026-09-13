# librei design notes

This file is the design authority for the core.
`include/rei.h` is the API contract.
This file holds the invariants that the implementation maintains.
Code comments reference this file.

## Wire format

Each struct in `include/rei.h` is the wire format.
Sizes are static-asserted, and alignment is 64 bytes.
Each shared hot word owns a full cache line, so writes from the producer and the consumer never share a line.
The layout is fixed: later phases add capability without moving anything.
`REI_ABI_VERSION` gates mixed builds.
Peers validate it at attach, before any thread reads or writes a shared atomic.
Bump it on each wire-format change.
Wire type tags (`rei_type_e`) sit in the same contract: int64 rides tag 32 (outside SEXPTYPE space), with INT64_MIN the missing sentinel.

## Statuses, not exceptions

Terminal transport states return `rei_status` values: `REI_FULL`, `REI_TIMEOUT`, `REI_CLOSED`, `REI_PEER_GONE`.
This keeps hot loops branch-cheap.
`REI_ERR` is a real error: a portable `rei_errcat` plus a formatted message.
The record lives on the handle and stays valid until the next call on it.
Handle-free entry points (regions, prune) use a thread-local slot.
Each binding maps the four terminal states to its own sentinel or condition values, and `REI_ERR` to its error hierarchy.
The mapping is per-verb: a full ring on send is a sentinel value for the binding, and a full injection ring at the submit deadline raises.

## Liveness lock is the death verdict

Each process holds an exclusive lock on a file for its full lifetime: an flock on POSIX, LockFileEx on Windows.
The kernel releases the lock on each exit path.
The lock is fd-scoped, so PID reuse cannot fake "alive".
The platform death listeners are wake triggers only: pidfd+epoll on Linux, dispatch sources on macOS, thread-pool waits on Windows.
The lock probe decides.
Linux requires kernel >= 5.3 (`pidfd_open`, no fallback): without a listener, the death of the host under a parked peer becomes a permanent hang.

## Parker protocol

Each waiting entity owns one epoch word.
The platform waiters are in `wait_linux.c` (futex), `wait_macos.c` (`__ulock`), and `wait_win32.c` (WaitOnAddress plus named events).
Each park site follows the same sequence: snapshot, announce, re-check, sleep-bounded.
An unpark that sees the announcement bumps the epoch after the snapshot, so the sleep returns immediately.
The re-check in the caller absorbs spurious wakes.
This handshake is the only guarantee against lost wakeups.
No watchdog sits behind it.

On POSIX, each park is timed: an untimed wait restarts silently under SA_RESTART, and a Ctrl-C then surfaces only at the next genuine wake.
An indefinite park uses `REI_PARK_NOMINAL_MS` and relies on directed unparks.

## Payload framing tiers

A frame is a 16-byte `rei_slot_hdr` plus payload bytes.
The tiers:

- `NIL`: an immediate empty value.
  No bytes move.
- `STR1`: a length-1 string.
  Bytes and encoding, no serialize.
- `RAWVEC`: the bare bytes of an attribute-free atomic vector.
- `RAWSPILL`: the same bytes out of line, in a channel arena chunk or a pool spill region.
- `INLINE`: a complete serialized stream.
- `ARENA`: one chunk in the channel spill arena (channel only).
- `SHM_RAW`: a serialized stream in a spill region.
  The consumer copies the bytes out before its consumer-done signal, so the region returns to the free list deterministically.
- `SHM_VEC`: a spill region that holds an REIH/REIS/REIL layout object.
  The consumer wraps a zero-copy view.
- `REF`: the `/rei_` identifier of an object already in shared memory.

The core moves opaque frames.
The tier contents are the binding's.

### Codec registry

The core is codec-agnostic: it never parses payload bytes.
But readers dispatch on the first byte of an INLINE stream, so the magic bytes need one owner.
This registry is it — a header comment alone would drift.

- `'B'` (0x42), `'X'` (0x58): R native serialize streams (binary / XDR).
- `'R'` (0x52, `REI_CODEC_MAGIC` in `rei_ext.h`): the rei compact codec.
- `'P'` (0x50, `REI_PYREI_CODEC_MAGIC` in `rei_ext.h`): the pyrei compact codec.

A binding introducing a self-describing stream claims its byte here first.
The drop's first-byte tags (`REI_DROP_*` in `rei.h`) are a disjoint context — drop region byte 0, never an INLINE payload — and share letters deliberately: `REI_DROP_R` is 0x52 as well, since 'R' denotes an R-binding payload in both.

## The binding seam

The core never sees a language object.
A binding registers its callbacks at create, attach, or join:

- `stage` and `read`: always.
- `exec`: on pool workers.
- `check`: the interrupt poll.
- `park`: for runtimes with a global lock.
- `sweep`: the idle cache drop.
- `drop`: the pin release.

The core copies the function pointers into the handle.
A hot-path call is one load plus a predicted indirect branch.
`stage_fn` receives the binding ctx alongside the handle; `exec_fn` receives a read ctx on the handle (the binding ctx rides it), so task-frame decode shares the collect path's region service.
The typedefs in `include/rei_ext.h` (the binding-author tier) document the full contracts.
Four of them carry the correctness guarantees:

- Staging is transactional.
  The core mutates no shared state before `stage_fn` returns.
  The core reclaims an arena chunk allocated mid-stage in FIFO order, like any other chunk.
  An uncommitted region checkout or pin rolls back at the next verb entry or at destroy.
  A binding can abandon mid-stage (a longjmp) and leave the handle consistent.
- A failed `read_fn` (a NULL return) consumes nothing — the slot stays for a retry.
  Setting `REI_READ_CONSUME` first flips that: the transport consumes exactly as on success (the channel head advance and its batched publication; the pool FREE transition, task-keeper drop, and producer-keeper wake) while the verb still returns `REI_ERR`.
  This is the foreign/corrupt-payload contract: an unreadable slot must not wedge the ring behind it.
  The binding carries its specific message in its own state; the core records no generic error for a consumed read.
- `exec_fn` must not abandon.
  The binding catches each task condition into the result sink.
  A worker that lets one escape degrades to worker death plus the reaper verdict.
  That is the hard-crash semantics, never the path for an ordinary task error.
- `check` runs at abandon-safe points only, once per spin or park iteration.
  A nonzero return unwinds the verb as `REI_ERR` with `REI_ERRCAT_INTERRUPTED` and consumes nothing.

### Staging policy

`rei_stage_raw` (dual-form in `rei_ext.h`; the slow path is `stage_raw.c`) is the core-owned raw-tier reservation: the RAWVEC / arena-RAWSPILL / region-RAWSPILL / flat-SHM_VEC cascade both first-party bindings stage by the same rules.
It composes the stager services and is valid only during `stage_fn`.
The churn signal is read at most once per stage, gated behind the zero-copy size gate.
A NULL return hands the object to the binding's serialized tiers: a reservation failure degrades, never errors.
Bare bytes carry no identifier, so the raw tiers pin nothing; the flat SHM_VEC reserve writes the REIH header before `rei_stage_retain_zc` stores the producer loan.
Object eligibility (which values are raw) stays binding-side, as do the string and list-tree layouts.

### Map morsel protocol

A binding's parallel map rides one fresh region per map call: a 128-byte header, the descriptor stream, an optional bare-bytes x section, the morsel state, and an optional template output area.
The protocol is core-owned (`morsel.c`, `rei_morsel_*` in `rei_ext.h`); the descriptor codec, the x-section element I/O, the batch loop, and the gather stay binding-side.
Map regions are private to a binding install, so the two first-party bindings share one layout and keep only their magic tags.

- One CLAIM word per runner ordinal packs `(generation << 2) | state`, so the lane claim and the generation fence are one atomic.
  A check-then-CAS would leave a TOCTOU window against reset's re-arm.
- The shared cursor is a relaxed ticket dispenser; ordering rides the task claim/publish chain.
  An overshoot of up to k morsels is harmless — a runner stops at its first exhausted issue.
- Batch sizing is AIMD: k targets a fixed batch duration, grows at most 2x per step, shrinks immediately on overshoot, and clamps to a cap that bounds lost-set coarseness.
- Completion is never recorded in the region.
  Runners publish their batch histories through ordinary results, and the lost set on worker death is arithmetic over them.

## Retain table

Payload lifetime is explicit, not GC-inherited.
Each handle owns a retain table of `{ region, pin, key, kind }` entries.
The transport commits an entry at publish.
It releases the entry at a consumer-done point: collect, slot reuse, the worker keeper sweep, or the channel head-advance reap.
The region half is core-owned.
The pin is an opaque binding token, and the core calls the binding's `drop` hook with it at release.
One uncommitted checkout (region plus pin) rides `fl->staging*`.
It rolls back at the next verb entry or at teardown, so a mid-stage abandon never leaks.

The free list of the producer recycles retired regions: power-of-two size classes, per-class and total-byte caps, largest-oldest eviction.
Steady-state spill traffic creates and unlinks no regions.
An SHM_VEC region recycles only after its zc refcount reaches zero.
A nonzero region waits in the lent-region ledger.
Busy paths sweep the ledger with a quota.
Idle paths and free-list misses sweep it in full.

If the consumer dies, its counts leak.
The death verdict on the producer then force-reclaims its lent regions.
REFHELD-flagged regions (their identifiers escaped by reference) leak and unlink instead.
On a ledger overflow, the entry closes the mapping but keeps the name, so a REF in flight can still resolve.
The unlink lands at handle teardown.

## Zero-copy views

A layout-eligible object past the floor of the binding stages as SHM_VEC.
The stage is one layout write into a spill region.
The consumer wraps the mapped pages as a view instead of copying.
The cross-process refcount lives in bytes [24-31] of the region header.
The offset macros are wire format (`REI_ZC_REFCOUNT_OFF` / `REI_ZC_FLAGS_OFF` in `rei.h`); the `rei_zc_rc` / `rei_zc_flags_` accessors are core-owned binding surface (`rei_ext.h`, dual-form).
The producer stores 1 at stage.
`rei_shm_open_view` adds 1 at the wrap of the consumer: open implies counted, so add-before-consumer-done is structural.
The `REI_OPEN_VIEW_NOCOUNT` form splits the two for a binding whose wrap can fail between map and count (an R ALTREP wrap can longjmp): open first, then `rei_zc_ref` at wrap, still before the consumer-done signal.
The release of the view subtracts 1.
The consumer maps page 0 read-write (the refcount word) and the rest read-only.

## Crash atomicity

Each cross-process claim is copy-then-CAS, so a death mid-operation cannot wedge a queue.
A worker announces its claim (`in_flight_rs`) before it CASes.
A reaper, serialized by the liveness lock, then fails exactly the tasks that the dead worker claimed.

## Pool mechanics

The pool has per-submitter SPSC injection rings, per-worker Chase-Lev deques, and result slots.
There is no dispatcher process.
A worker seeks work in tier order: fairness tick, own deque, random-victim steal, injection scan.
A nested submit on a worker handle pushes to the deque of that worker.
A full deque runs the task inline.
A worker blocked in a nested collect helps (executes or steals) instead of sleeping.
A task error never crosses as the caught condition itself.
The ERR publish carries the flattened envelope of the binding, framed so that the publish cannot raise: fail the task, never the worker.

## ABI discipline

The API has three tiers (the CPython PEP 689 model):

- `include/rei.h`: the stable consumer API.
  Handles are opaque.
  Nothing public passes or returns a struct by value.
  Extensible structs carry a size field.
  Each public operation is a real exported function (`REI_API`).
  The shared library carries a soname that tracks `REI_VERSION_MAJOR`; the stable promise starts at 1.0.
- `include/rei_ext.h`: the binding-author API, installed but version-pinned per minor release.
  It may change on any minor bump, without deprecation, ever — including after 1.0.
  Consumers are language bindings; they rebuild (or re-vendor) per minor release.
  Real exported functions are the rule; `static inline` is permitted only for the three dual-form offset-math accessors (`rei_parker_snapshot`, `rei_zc_rc`, `rei_zc_flags_`), each always paired with a same-named exported symbol (`src/ext.c`) so FFI consumers see everything.
  `struct rei_shm_s` is exposed here, layout-pinned per minor release: bindings dereference it on hot paths where an accessor call would not amortize.
- `src/internal.h`: private, never installed.
  The handle struct definition, spin machinery, and spill/ledger/open-cache internals live here.

Builds use `-fvisibility=hidden`.
Two contracts are versioned, both documented in the `rei.h` preamble: the wire-format `REI_ABI_VERSION` and the soname.
The wire-format structs in `rei.h` are the preamble-versioned contract, not soname-frozen.
