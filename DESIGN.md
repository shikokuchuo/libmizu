# libmizu design notes

This file is the design authority for the core.
`include/mizu.h` is the API contract.
This file holds the invariants that the implementation maintains.
Code comments reference this file.

## Wire format

Each struct in `include/mizu.h` is the wire format.
Sizes are static-asserted, and alignment is 64 bytes.
Each shared hot word owns a full cache line, so writes from the producer and the consumer never share a line.
The layout is fixed: growth adds capability without moving anything.
(One exception, pre-release: the cross-language work repurposed reserved bytes — the channel entity blocks' identity words, the pool header's `worker_ident`, and the layout headers' flags and validity words — recorded in the CHANGELOG; `MIZU_ABI_VERSION` stayed 1, as peers are same-build.)
`MIZU_ABI_VERSION` gates mixed builds.
Peers validate it at attach, before any thread reads or writes a shared atomic.
Bump it on each wire-format change.
Wire type tags (`mizu_type_e`) sit in the same contract: int64 rides tag 32 (outside SEXPTYPE space), with INT64_MIN the missing sentinel.

## Statuses, not exceptions

Terminal transport states return `mizu_status` values: `MIZU_FULL`, `MIZU_TIMEOUT`, `MIZU_CLOSED`, `MIZU_PEER_GONE`.
This keeps hot loops branch-cheap.
`MIZU_ERR` is a real error: a portable `mizu_errcat` plus a formatted message.
The record lives on the handle and stays valid until the next call on it.
Handle-free entry points (regions, prune) use a thread-local slot.
Each binding maps the four terminal states to its own sentinel or condition values, and `MIZU_ERR` to its error hierarchy.
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
An indefinite park uses `MIZU_PARK_NOMINAL_MS` and relies on directed unparks.

## Notify endpoints

A waiting entity can own a notify endpoint: a named, pollable wake object the publisher signals at the same fenced checkpoint where it tests the parked announce.
On Linux the endpoint is an abstract-namespace datagram socket (no filesystem entry, no unlink races, auto-cleanup on close); on macOS a bound filesystem datagram socket under the liveness dir (a path past the `sun_path` budget fails the arm with a recorded error); on Windows an auto-reset named event (`SetEvent` while signaled coalesces, the wait consumes it).
Notify endpoints are a second wake-trigger class: like the death listeners, they are triggers only, never a verdict.
The consumer's drain-and-recheck decides.

The arm is the parker protocol's announce/re-check transplanted: arm stores the flag with a seq_cst RMW, fences, and reports availability, so either the publisher sees the flag or the armer sees the data.
The flag is edge-triggered.
It rides the word the publish already loads and tests — the channel's `parked` word as `MIZU_WAIT_NOTIFY` beside the park announce, the pool's `waiter_slot` as `MIZU_WS_NOTIFY` beside the submitter index — so the unarmed publish path runs the same load, the same compare, and the same not-taken branch as without the feature.
The publisher clears the bit as it signals (the winner of the clear sends), so one signal moves per idle-to-data transition, never one per publish.
The pool coalesces per submitter: the slot's gate selects the endpoint wake, and the first publisher past it clears the submitter's `notify` word — one signal per burst, however many workers publish.
Spurious and stale signals are absorbed by the drain-and-recheck; a datagram queued while disarmed is dropped at the next arm.

A terminal that publishes nothing still wakes an armed consumer.
The channel's watch chains the peer's death watch onto the same callback, and the close paths signal the endpoint alongside the parker unpark.
A pool submitter needs no chain: a worker death is failed into result publishes by the reaper, which reach the armed submitter through the ordinary `waiter_slot` path.
The watch itself is a persistent registration on the death-listener machinery (Linux's epoll thread, macOS dispatch sources, Windows thread-pool waits), firing a pure-C callback off-thread; a hard kill leaves the macOS socket path as residue beside the `.live.*` files — the reaper's exact region grammar never unlinks suffixed names, and unlink-before-bind absorbs the residue.

## Payload framing tiers

A frame is a 16-byte `mizu_slot_hdr` plus payload bytes.
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
- `SHM_VEC`: a spill region that holds an MIZH/MIZS/MIZL layout object.
  The consumer wraps a zero-copy view.
- `REF`: the `/mizu_` identifier of an object already in shared memory.

The core moves opaque frames.
The tier contents are the binding's.

### Codec registry

The core is codec-agnostic: it never parses payload bytes.
But readers dispatch on the first byte of every serialize tier (INLINE, ARENA and SHM_RAW alike), so the magic bytes need one owner.
This registry is it — a header comment alone would drift.

- `'B'` (0x42), `'X'` (0x58): R native serialize streams (binary / XDR).
- `'R'` (0x52, `MIZU_CODEC_MAGIC` in `mizu_ext.h`): the mizu compact codec.
- `'P'` (0x50, `MIZU_PYMIZU_CODEC_MAGIC` in `mizu_ext.h`): the pymizu compact codec.
- 0x80: a pickle stream, protocol >= 2 (the PROTO opcode; earlier protocols are text and never staged).
- `'I'` (0x49, `MIZU_INTEROP_MAGIC` in `mizu_ext.h`): an interchange stream — the cross-language wire format of the Interchange codec section.

A binding introducing a self-describing stream claims its byte here first.
The drop's first-byte tags (`MIZU_DROP_*` in `mizu.h`) are a disjoint context — drop region byte 0, never an INLINE payload — and share letters deliberately: `MIZU_DROP_R` is 0x52 as well, since 'R' denotes an R-binding payload in both.

The wire type tags (`mizu_type_e` in `mizu.h`) ride this registry's discipline: the numbers descend from R's SEXPTYPE space by history, but libmizu owns and freezes them from the first release — a future SEXPTYPE joins the wire only by an explicit allocation in `mizu.h`, as `INT64 = 32` already did.

### The keeperless claim

`MIZU_AUX_F_KEEPERLESS` (`UINT64_C(1)`) is bit 0 of the INLINE aux word: a stager-authored claim that staging this frame committed no retain-table entry.
The core reads the claim, never verifies it; clear is always correct (at worst a spurious keeper-sweep wake).
A wrongly-set bit defers reclaim, never frees early: the keeper drop is recorded unconditionally at collect — only the cross-process wake is gated.
Only INLINE has a free aux bit (ARENA: chunk offset; SHM_RAW: stream length; RAWVEC/RAWSPILL: type tags; SHM_VEC: layout tag + used bytes; REF is keeper-ful by definition; NIL/RAWVEC/STR1 are kind-statics), so the claim is INLINE-only and every other kind tests keeper-ful unless immediate.
The channel's reap gate is a pinned-keeper counter and is untouched — the win is pool collect only.

## Interchange codec

The cross-language wire format — what a value looks like when writer and reader are different languages — in one section: the identity negotiation, then the byte format.
This section is the authority for the bytes.

### Languages, capabilities and the identity word

Three small registries ride the identity word:

- **Languages**: 0 = none (no peer attached yet; a pool no worker has joined — never a stored binding identity), 1 = bytes (the core's built-in bytes binding and the core's test bindings — the mandatory rule's answer for a languageless consumer), 2 = R, 3 = Python.
  Used by the identity word, the pool's worker identity word, the task tag's target byte and submitter identity, and each binding's handle-level `peer_lang`.
- **Reader capabilities**: a 32-bit mask, one bit per layout or format extension a reader implements beyond the baseline (MIZH atomic and INT64 views, `'I'` format 0x01 as specified below, its attr shape whitelist included).
  `MIZU_CAP_MIZS` (bit 0): reads MIZS layouts.
  `MIZU_CAP_ATTRS` (bit 1): reads an `'I'` attribute blob on a layout root or MIZL leaf, and homes the whitelisted shapes there.
  `MIZU_CAP_MIZL` (bit 2): wraps a generic MIZL tree as a `list` of views — and as a `dict` when the reader also sets `MIZU_CAP_ATTRS`, since a names blob is an attribute blob.
  `MIZU_CAP_TASKREF` (bit 3): reads 0x13 ref leaves — task arguments by reference (task streams and the map descriptor, which shares the value grammar).
  `MIZU_CAP_MIZL_REF` (bit 4): reads MIZL remote leaves — directory tag 33.
  `MIZU_CAP_MIZL_TSN` (bit 5): reads MIZL INT64 leaves carrying a `tscale` attribute — the frame writer's verbatim nanosecond temporal form (the Arrow frame writer subsection).
  `MIZU_CAP_MIZL_U64V` (bit 6): reads MIZL INT64 leaves carrying a `u64` attribute — the frame writer's verbatim uint64 form (the Arrow frame writer subsection).
  `MIZU_CAP_LABELED` (bit 7): reads the labeled attr shapes — a 1-D `names` vector, a dim + `dimnames` array (the `table` class included), and the extended frame dict (a class vector ending `"data.frame"` plus extra attributes).
  A binding sets the bits for what its reader implements in that release.
  The rest are reserved: a later `'I'` tag or a later attr shape takes a bit, so writers emit it only to readers that set it.
- **The identity word**: `MIZU_IDENT(lang, caps)`, a u64 with the language in bits 0–7, the 32-bit capability mask in bits 32–63, and bits 8–31 reserved: written zero, and *ignored* by readers.
  The identity word is a negotiation word, so a later field there (a format-version advertisement, say) needs no reader taught to skip it.
  The pool word's exact-match join still compares the whole word: one binding build carries one word.

Three rules govern the capability bits:

- **Readers ignore capability bits they do not know; they never reject them.**
  This is the opposite of the reader's unknown-tag rule below, and it is deliberate.
  A peer advertising a capability the writer does not know is harmless, because the writer never stages a layout or tag it does not implement.
  Absent means unsupported: channel and pool regions are created fresh and zero-filled (`mizu_shm_create_stack`: `ftruncate` on a fresh object / a pagefile-backed `CreateFileMappingA`), so a reader that has not set a bit is sent a copy.
  This is what lets the two bindings land a reader feature in either order — the reader flips its bit, and every writer follows without a change.
- **Allocation policy.**
  One bit per reader feature a writer must gate on.
  Features that always land together in every binding share a bit.
- **A bit names a byte layout, not a feature.**
  The mask is the only version gate the layouts have — `MIZU_ABI_VERSION` stays 1 while peers are same-build, and a region reached by REF sits behind no preamble at all — so an incompatible change to a layout, tag or shape a bit gates never rides the same bit.
  It allocates a new bit and retires the old, which is never reassigned: a peer that set the old bit against the old geometry is sent the old form or a copy, never the new bytes under the old promise.
  The MIZS re-shape (16-byte entries → the Arrow block) is the precedent: it landed before any bit existed, and would take a fresh bit today.
  From the first tagged release the ABI bump rule resumes for the preamble-gated structs, and the bit discipline is the versioning for everything the mask gates.

Language and capability values are additive, and language values are append-only: a value, once shipped, is never reassigned.
The byte is equality-compared and the wire layout is unchanged by a move, so the ABI gate cannot catch a reassignment — it would fail behaviorally, as false join rejections across mixed builds.
A new binding takes the next free value, whatever the ordering aesthetic.
A worker cannot join a pool of another language (the pool word's exact-match join), so a foreign byte reaches a worker only through a task tag's target byte, which it fails with an error stream "task language mismatch" and never executes.

Each channel side publishes its word in its entity block (`MIZU_ENTITY_IDENT` in `mizu.h`): the host at create, the peer at attach before `ready_set`.
A pool has one word in its header (`worker_ident` in `mizu.h`): the first worker join CASes its binding's word in, an exact whole-word match joins, anything else fails.
The word is read through the mapping (it is written after create, so a validated handle copy is stale), is never reset, and outlives any worker generation: homogeneity is per pool, not per worker generation.

Format words reject what they do not know; negotiation words tolerate it: the region header flags word (`mizu.h`, bytes [32-35]) and the task tag's flags reject a set bit they do not know, where the identity word's reserved bits 8–31 are ignored.

### The format

The err tag's internals, the layout attribute blob's `'I'` form, the task tag's internals, the ref leaf (task arguments by reference) and the MIZL remote leaf (a per-column REF) are normative below.

Invariants — the spine every interop decision hangs from: (1) a reader never meets a stream it cannot identify (the first-byte registry; an unlisted byte is an informative decline); (2) a writer never sends a value its peer cannot read (the identity word and capability mask; a decline fails at send); (3) same-language handles keep their private codecs (the fastest and most complete path; interop *value* streams appear on foreign handles only — the same-language uses are the layout attribute blob, an `'I'` encoding inside a region, and the err stream a peer shim sends for an uncaught error on every channel, neither a value the stage hook chose, plus one that is: mizu's small-table `'I'` form, an Arrow table at or under the size gate written as one stream straight off the Arrow buffers and read back through the validating foreign-shape builder, kept byte-identical with the foreign path — pymizu has no such use, its same-language small `Frame` still pickles); (4) no silent downgrades (an unknown tag or a reserved flag bit is an informative error, never a misparse or a coercion); (5) relays shift containers, never values (each far-side home shift is documented, and the corpus pins every one).

**The MIZL remote leaf (directory tag 33).**
A MIZL directory entry whose S4-masked sexptype is 33 is a remote leaf: the column lives in another region and crosses by reference.
`data_offset` / `data_size` hold the identifier span — the view layer's identifier string, the same byte form as the 0x13 ref leaf's: the region name, optionally `name[i,j,...]` with 1-based decimal indices for a leaf inside another region's tree.
The span keeps the 64-byte `data_offset` alignment rule, and `data_size` is the identifier byte count, 1–255 (the 0x13 form's u8 bound, reused).
`length` is the referenced column's element count *as resolved* and `attrs_size` its attribute-blob size as resolved (0 when attribute-free): neither is checked against `data_size` (they describe the remote vector, not the local span), so the elt-size and string-geometry body checks skip tag 33, and the shared prologue's `attrs_size > data_size` reject exempts it — a 30-byte identifier span can reference a factor leaf with a 200-byte blob.
The S4 bit is never set on a remote leaf — the referenced column's own layout carries its S4 bit — and a reader rejects a tag-33 entry with the bit set (format-word discipline, no silent downgrades).
The validity pair is `{0, -1}` when the referenced column is known NA-free, else `{0, 0}` (unknown — the reader takes NA-ness from the referenced region's own validity state); `{0, -1}` is claimable only where the referenced form records known-NA-free itself (an MIZH header pair or an MIZL atomic leaf's table pair), so STR and serialized references always claim `{0, 0}`, and a nonzero validity offset rejects — a bitmap offset is region-local and cannot describe a remote column.
The header validity pair describes the *local* leaves only, and the exclusion is forced, not cosmetic: `mizu_mizl_check` bounds the header `vcount` by the sum of local atomic leaf lengths (tag 33 adds nothing), so tallying remote nulls into the header emits a region that checks as corrupt (an all-remote frame with nulls — columns matched from different exports, where the whole-frame fast path cannot fire — would stamp `vcount > 0` with `sum == 0`).
A remote column contributes no bitmap and no count to the header validity; its row, when a table exists, is the `{0,0}` / `{0,-1}` claim alone, and the writer's clean-run collapse stamps a header `{0, -1}` only when local nulls are zero *and* every remote column legitimately claims `{0, -1}` — otherwise the table is emitted, since a collapsed header makes a remote entry's claim read `{0, -1}` and the header must never speak for a remote column.

Validation at resolve: a reader meeting tag 33 resolves the identifier through the same machinery as a path REF (open the region, walk the optional path) and validates the entry's claims against the resolved leaf — element count, attrs-blob size, and NA-free-ness when `{0, -1}` is claimed.
A `{0, -1}` claim against a referenced form that records no NA-free marker (a bare MIZS root, a serialized reference) declines as corrupt — the writer rules above forbid the claim ever being legitimate, so this is never an unvalidatable pass.
A dangling identifier, a parse failure, or any mismatch declines as a corrupt region — the same shape as an unknown tag (meeting one unadvertised is the corrupt-or-newer decline).
Emission gates on `MIZU_CAP_MIZL_REF` (bit 4) — the versioning rule's new-bit-for-a-gated-shape case: a tree the reader cannot walk fails the whole receive, unlike the ref leaf's per-task informative decline — and the gate is per frame: a frame with no remote leaves keeps the baseline conjunction, the bit joining only when the writer would emit remote leaves, so a peer short of it gets full layout leaves for every column, never a partial-remote tree.

Byte 0: `'I'` (0x49). Byte 1: format version (0x01). Then exactly one value: a stream ends where its value ends, and bytes past it are malformed. The cursor's finishing check compares its position to the frame's `len` (the slot header's for INLINE, `aux` for SHM_RAW, the header-located `attrs_size` for a layout attribute blob), so a task with one field too many at top level fails there instead of leaving the field unread. The version byte moves only for a change to these two header bytes or to the *meaning* of an existing tag. Growth is by tags under the unknown-tag rule below, so 0x01 is the baseline every reader implements, and it may never move. A reader meeting a version it does not know declines with the same informative shape as an unknown tag ("interop format version 0xNN — the peer uses a newer format"), never a bare corrupt-stream error, and a writer never emits a version its peer has not advertised: a bump is a reader feature, so it takes a capability bit (the allocation policy above) or a field in the identity word's reserved bits 8–31, which readers already ignore. Little-endian throughout (all supported platforms are LE; the spec makes it explicit for the cross-language contract). Multi-byte fields sit at unaligned offsets — every count and value follows its tag byte directly, the task header's u64 identity is at bytes 7–14 behind its u16 flags, and the err tag's u64 index follows the u16 flags — so readers `memcpy`, never cast. Every element count is u64: the vector tags 0x06–0x0b, the containers 0x0c–0x0d, i64v, the 0x05 bytes scalar, and the attr tag's dict (a factor or a frame inherits it through its parts). Every count is bounds-checked before anything is allocated from it: a vector or bytes count against its element width and the bytes remaining in the stream, and a list, dict or strv count against the remaining byte length alone — every element costs at least one byte (a tag, or an i32 length), so `count ≤ remaining` is the pre-allocation bound the cursor enforces at the container begin, and a builder allocates only behind it. A count that fails it is truncation, the cursor's error, never a builder's out-of-memory. Only the byte length of a single string — the 0x04 scalar and the strv per-element length — is i32, with −1 = NA: R caps one string at 2^31 − 1 bytes, and a string vector pays that length once per element. A vector *nested* in a list or dict crosses inside the `'I'` stream whenever its tree takes the copy tiers (below the zero-copy floor, or to a peer without `MIZU_CAP_MIZL`), and a large stream rides SHM_RAW's u64 `aux` — a u32 count would have been the only cap on nested data. Recursive with a depth cap of 64. Past the cap the writer declines with reason `depth`, like any other non-qualifying value: a send-time raise, since only foreign handles write interop. It never writes a partial stream. A reader meeting an unrecognized tag declines with an informative error naming the tag byte ("unsupported interop tag 0xNN — the peer uses a newer format"), never a bare corrupt-stream error. Pinned now, this is what lets version 0x01 grow tags later without a wire change.

| Tag | Wire form | R | Python |
|---|---|---|---|
| 0x00 nil | — | `NULL` | `None` |
| 0x01 lgl1 | u8: 0/1/2=NA | `logical(1)` | `bool` (NA → `None`, documented lossy) |
| 0x02 int | i64; INT64_MIN = NA (this stream has no validity section — the sentinel is the whole null record; see Validity as data) | `NA_integer_` ↔ INT64_MIN; otherwise `integer(1)` if in (INT_MIN, INT_MAX], else `integer64(1)` (bit64 layout). −2^31 is `NA_integer_` in R, so it takes the integer64 home like any value past int32. R never writes 0x02 from an integer64 (0x0e) | `int`; INT64_MIN → `None` (documented lossy, as lgl1 — a Python −2^63 arrives in R as `NA_integer_` and relays back as `None`: the `MIZU_TYPE_INT64` sentinel collision, documented) |
| 0x03 real | f64 bits (`NA_real_` crosses as its NaN payload; no validity section here either — see Validity as data) | `numeric(1)` | `float` (NaN payloads bitwise-preserved; `NA_real_` arrives as NaN) |
| 0x04 str | i32 len (−1 = NA) + UTF-8 | `character(1)`; `NA_character_` ↔ len −1; written as UTF-8 (latin1 translates — `identical()`-clean), CE_BYTES and invalid UTF-8 decline; read as CE_UTF8 | `str`; NA ↔ `None` (documented lossy, as strv) |
| 0x05 bytes | u64 count + bytes | `raw` (read only: R writes every raw vector as 0x0a rawv under the atomic-vector rule — raw has no scalar form) | `bytes` (written by exact-type `bytes`; a relayed `bytes` returns as a uint8 array, the home a top-level `bytes` already has on the RAW wire type — documented) |
| 0x06–0x0a lglv/intv/realv/cplxv/rawv | u64 count + raw element bytes | atomic vector (NA sentinels per `MIZU_TYPE_*`) | numpy array; lglv and intv follow the fused-scan rule (one pass settles NA-ness and the home dtype) on every copy tier — lglv: `bool_` if no NA, else int32 with `INT_MIN`; intv: int32 if no `INT_MIN` element, else float64 (every int32 is exact there) with R's `NA_real_` payload for each missing; no numpy → memoryview/list (sentinels in place, documented) |
| 0x0b strv | u64 count + per-elt (i32 len, −1 = NA) + UTF-8 | `character(n)` | `list[str\|None]` |
| 0x0c list | u64 count + elements | unnamed `list` | `list` |
| 0x0d dict | u64 count + (key, value) pairs — each key a bare string (i32 length ≥ 0 + UTF-8, no tag byte; −1 is malformed), each value a tagged item; keys unique — a duplicate is malformed, and the *builder* rejects it (the cursor is allocation-free and holds no key set), never last-wins: the writer never emits one, so a duplicate on read is a torn or mis-written stream, and a silent overwrite would be the downgrade invariant 4 forbids | named `list` (names non-NA, writable as UTF-8, and unique after translation — else decline) | `dict` (str keys — else decline) |
| 0x0e i64v | u64 count + i64 elements (NA = INT64_MIN) | `integer64(n)` (bit64 layout), any n including 1 — the only tag R writes for integer64 | numpy `int64` (a sentinel-carrying read warns — the warn-only rule: a loss event warns, never errors) |
| 0x0f attr | one complete value (its own tag, never 0x0f) + the attributes as one ordinary tagged 0x0d dict item — the 0x0d byte, u64 count, (key, value) pairs in stored order; there is no bare untagged form, so the cursor reads it through its container path | the value with the attributes applied, class last (`Rf_classgets` — the `mizu_view_set_attrs_from` discipline); on a foreign handle a whitelisted shape only: plain `factor` (intv codes, 1-based, `INT_MIN` = NA; attrs exactly `levels`, `class`) or plain `data.frame` (shape rules below), a plain dim array (attrs exactly `dim`), a `Date` (realv days, attrs exactly `class`), a `POSIXct` (realv epoch seconds, attrs `class` plus an optional `tzone`), or a `difftime` (realv in the declared units, attrs exactly `class`, `units`), plus the `MIZU_CAP_LABELED` tier: a named vector (attrs exactly `names`), a dim array with `dimnames` (and an optional `"table"` class), and the extended frame (a class vector ending `"data.frame"` plus extra attributes) — the whitelist bullets below | per shape: `data.frame` → `pymizu.Frame` (reader notes below; column lengths validated); `factor` → `list[str\|None]` on its own, an Arrow dictionary column inside a `Frame`; dim array → an ndarray of the value's dtype built `order="F"` (R column-major and numpy Fortran order name one byte layout), the labeled forms a `DimArray`; Date → `datetime64[D]`, POSIXct → naive `datetime64[us]` UTC (a named `tzone` riding only a `Frame` column's metadata), difftime → `timedelta64[us]`; any other attribute set → the informative no-home error naming the class and attribute names |
| 0x10 cplx | f64 re bits + f64 im bits | `complex(1)` (`NA_complex_` bitwise — the 0x03 rule) | `complex` (NA arrives as `complex(nan, nan)`) |
| 0x11 err | u16 flags (bit 0 = index present; other bits reserved zero, a set one is malformed) + [u64 index] + three bare strings (i32 length ≥ 0 + UTF-8, no tag byte): type, message, detail | a `mizu_error_remote` condition (fields `remote_type`, `message`, `detail`, and `index` when present) — a value, legal at a stream's top level only | `TaskError` / `MizuError` with `remote_type` / `remote_traceback`; top level only |
| 0x12 task | u8 target + u8 kind + u16 flags (reserved zero; a set bit is malformed — the field exists so a later header addition is a flag, never a moved identity) + u64 submitter identity, then the fields of its kind in registry order (the task kind registry below), each a tagged item, bare — no count and no wrapping container | not a value: the exec hook takes it at the top of an entry payload and the map descriptor reader as the descriptor's first element; every other builder reports "a task is not a value" | same |
| 0x13 ref | u8 name_len (1–255 — 0 is malformed) + the identifier bytes: the view layer's identifier string (a region name, optionally name + path for a view into a tree), opaque to the cursor, which checks the length and bounds only | a view over the referenced region — the task-stream exec decode, the map descriptor reader and the collect-side result reader take it; every other builder declines informatively | same |

Tagged versus bare: every value and every task field is a tagged item. The only bare fields are strings whose position fixes their type — dict keys and the err tag's three strings — so the cursor never meets an untyped value, and neither can be NA.

Two tags are not ordinary values. The err tag (0x11) is a value — a received error is something user code decides to raise — but it is legal only as a stream's top-level value: the cursor rejects it nested, since nothing nests one. The task tag (0x12) is not a value at all: the cursor yields its fields wherever it appears — counting them as one element of kind-determined arity, so a task nests as a single list element — and three builders have a case for it — the worker's exec hook at the top of an entry payload, the map descriptor reader as the descriptor's first element, and the inspection hook (a test path: it builds, never evals). Every other builder reports "a task is not a value" for it, the same informative shape as an unknown tag. Both ride `'I'` rather than magics of their own: one magic means one cursor, one corpus grammar and one version story for every cross-language byte.

**The err writer is bounded and cannot fail.**
Each of the three strings truncates at a UTF-8 boundary past its share of the slot's inline budget: type past 128 bytes, message past half the budget (both bindings' task-flatten share), detail past what remains of the budget after the first two, less the 25-byte worst-case overhead (magic, version, tag, flags, the index, three counted lengths).
The frame therefore fits the slot by construction: the writer stamps INLINE directly, with the keeperless claim set — the stream pins nothing.
These properties — bounded by truncation, always inline, a writer that cannot fail (an ERR publish fails the task, never the worker) — come from the special-purpose writer, not the opening byte, which is why the err tag needs no magic of its own.
Writers: a channel's peer shim, for an uncaught peer-source error, on every channel — same-language included, so the value a host receives depends on neither side's language; the ERR publish of a foreign-submitter task, "result not portable" and "task language mismatch"; and per-element map errors (the index field).
A channel receives an error stream as a **value**, not a raised condition (sentinel discipline: transport states are values, payloads are values — user code decides to raise), and the same err-stream framer serves the pool's ERR publish through `inline_n`, whose writer faces the same cannot-fail rule.

**Task kind registry.** The kind byte selects an ordered field list, and the list's length is the cursor's arity for the tag: a task owns exactly that many following items, so it nests as one element and a shortfall is truncation. The registry is the spec's, not the cursor's — the table below is what the cursor's arity table implements, and a kind the cursor does not know is the informative decline ("unknown task kind 0xNN — the peer uses a newer format"). Adding a kind is a registry row plus the cursor's table entry, which rides the same re-vendor as the builders every binding needs for it anyway. The arity is derived, never carried: a count on the wire would duplicate what the kind already fixes and move the shape check from the one fuzzed cursor into each binding's builder, and a wrapping 0x0c list would still leave the cursor a task-specific rule (a task owns one following item) for nine bytes — so the fields ride bare, as the header does. Kind values are append-only, like language values.

| Kind | Name | Fields, in order | Emitted by |
|---|---|---|---|
| 0 | qualified name | code (0x04 str), positional (0x0c list), named (0x0d dict) | Pool tasks |
| 1 | source | code (0x04 str), positional (0x0c list), named (0x0d dict) | Pool tasks |
| 2 | runner | region_name (0x04 str), generation (0x02 int: the runner ordinal in the high 32 bits, the morsel generation in the low 32 — one i64 because the registry row has three fields and both numbers ride it; a counter never approaches 2^63), seed (0x00 nil, or 0x0e i64v[2] = (seed, offset)) | Map runners |

The field *tags* are the builder's check, not the cursor's: the cursor counts items and validates each as an ordinary value, and a field of the wrong tag fails the task in the exec hook with the informative error stream. The cursor parses every registry row unconditionally — the table's last column names the feature that emits the kind.

Reader/writer notes: the R reader constructs integer64 the way `mizu_wire_alloc()` does (tag-driven allocation with the class re-applied), keeping one int64 construction site. Factor codes cross in R's own 1-based layout (the intv form verbatim — memcpy both ways on the R side); the Python side does the ±1 in its dictionary-column construction loop.

**The task tag's header fields sit at fixed offsets for a reason.**
Byte 2 is the tag, byte 3 the **target** language (the worker language the task is written for, from the language registry — the misroute guard), byte 4 the **kind**, bytes 5–6 the u16 **flags** (reserved zero) and bytes 7–14 the u64 **submitter identity** (the submitting binding's whole identity word, copied verbatim).
The worker reads the tag and target bytes without opening a cursor, and the offsets are stream offsets, not entry-payload offsets: a task stream past the inline budget spills to a named SHM_RAW region like any pool payload (the ordinary SHM_RAW retain; no keeperless claim — INLINE pins nothing, and the claim would be inert on a task entry either way, the collect-side gate reading only result-slot headers), so the exec hook resolves the entry first — an INLINE entry's payload in place, a SHM_RAW entry through the region open a value read would perform — and reads bytes 2 and 3 off the resolved bytes.
A target byte that differs from the worker's own language fails the task with an error stream "task language mismatch" and never executes.
The check is defense only: joins are homogeneous and the target is always the pool word's language byte, so no well-formed path produces a mismatch — it is one byte compare, and it is what stands between a torn or mis-stamped task stream and executing code written for another language.

The submitter identity carries two things the target byte cannot.
Its language byte (bits 0–7) is the ERR-format and result-policy key: a submitter of the worker's own language keeps the pool's private ERR format and result staging, and the target byte cannot serve here — after the misroute check it always equals the worker's language, so it cannot tell a foreign submitter from a same-language one using the spec verb on its own pool.
Its capability mask (bits 32–63) drives the worker's zero-copy filter on the result.
A foreign submitter's ERR publishes as an error stream (the err tag above), and its result stages under the foreign writer policy — a non-portable result fails the task with an error stream ("result not portable", naming the value's type) instead of crossing as a value the submitter cannot read.
The identity rides the task stream rather than the core because a pool can have submitters of several languages, each with its own reader; it carries the whole word under the same "ignore unknown bits" rule as a channel.
A malformed stream — a wrong field tag or count for its kind, a torn header — fails the task with an informative error stream, never a bare corrupt-stream error and never worker death: the decode sits inside the worker's per-task error containment.

For kinds 0 and 1 the code is a 0x04 string, the positional arguments a tagged 0x0c list and the named arguments a tagged 0x0d dict — both always present, either may be empty.
An R mixed call maps unnamed arguments to the list and named ones to the dict, matching Python's `*args, **kwargs`.
There is no name registry: a qualified name resolves through the worker's own namespace machinery (an R package's namespace, a Python module).
Source kind evaluates the source in a fresh namespace with the named arguments bound as names, and the result is the trailing expression's value if the source ends with one, else NULL/None — one rule, both languages.

A task frame that is not a task stream and whose first byte is another binding's magic — `'P'` or pickle's 0x80 at an R worker, `'R'` or an R_Serialize `'B'`/`'X'` at a Python worker, the same registry test the channel read hooks run — publishes a neutral error stream ("task in a foreign private codec") instead of a private ERR the submitter could not read.

**Task arguments by reference: the ref leaf.**
A 0x13 ref leaf in a task stream's argument fields carries an argument by reference instead of by value: the identifier of an object already in shm.
Two emission cases share the one leaf and the one resolve: a received view re-sent as an argument (the emitter ORs REFHELD into the region's flags word at emit — the identifier has escaped by reference), and a fresh layout-eligible value past the zero-copy floor (one layout write into the stage's single spill checkout, the producer-loan store, the leaf carrying the fresh region's name).
The reader resolves the identifier to a view with the counted add inside its decode, before the claim completes; which loan the emitter held is the only difference between the cases.
The leaf is an ordinary value in the cursor's grammar — it nests in the argument list and dict like any node — and builders without a case for it (the channel value readers, both directions) decline informatively and consume the payload, never a wedge.
Growth is by tags, so the version byte stays 0x01: a reader without the tag declines informatively, which for a task stream fails the task with an error stream — no wedge.
To spare that round trip a submitter emits 0x13 only to a pool whose worker word sets `MIZU_CAP_TASKREF`: a spec carrying a ref-eligible argument to any other pool declines locally at submit, naming the remedy.
A ref emission also filters on the peer's capability mask per the view's layout (the zero-copy-tier filter's behavior): an attributed or MIZL view refs only to a peer that can read it, else the value write — a missing reader feature declines locally, insufficient layout caps degrade to a copy.
The fresh-value case rides the staging seam's single checkout: a stream carrying a checkout must otherwise fit the inline budget — it cannot also spill SHM_RAW.
So a spec whose pre-scan finds a ref-eligible argument runs the size pass ahead of any byte write, picks the first layout-eligible argument in document order (positional, then named, depth-first) such that the remainder of the stream fits the inline budget, and records the choice for the write pass; with no such candidate the write runs by value from the start and spills SHM_RAW as ever.
Eligibility is a pure function of the value, the floor and the handle's churn flag, and no verbs run between the passes, so the two passes cannot disagree.
A checkout failure mid-write abandons the stage and retries with the argument in its copy form — never a partial stream (the core rolls an uncommitted checkout back at the next verb entry).

The retain protocol mirrors the REF tier's handoff on both sides.
The submit side pins: a REF emission leaves the emitter's own view object holding the region's count, and without a pin the region could recycle under an identifier in flight — so the stage pins the emitting spec (every view the argument trees carry) into the retain table, and a frame that can carry a ref never stamps the keeperless claim (a pinned loan is a retain-table entry, unlike a checkout loan).
The worker's counted add lands inside its decode, before the claim completes; the pin releases at the claim-side release point, and the count never reaches zero in between.
The worker side releases deterministically: a GC-managed binding would otherwise linger the decoded argument views' adds across tasks, so the exec hook records every by-reference emission its outcome write makes — the result write or the ERR flatten, one list, every emission funnelling through the one mark from whatever graph position — and, once the outcome stream is written, force-releases each decoded argument view absent from the list.
A view the write emitted keeps its loan: the publish pins the result value, and the submitter's collect-side add lands before the worker's consumer-done releases that pin — the same handoff on the worker side.
A worker death mid-task leaks one count per decoded ref — the consumer-death rule of the retain table — and the regions are REFHELD, so a later producer death verdict leaks and unlinks rather than recycles into reuse.
The map descriptor shares the value grammar, so a view x crosses to foreign workers by the same leaf: the worker's map-context cache holds the resolved view between morsels, and the idle sweep or teardown releases it.

The attr tag is the one form for every attributed value, inline and zero-copy alike: the same 0x0d attribute dict is the attribute blob of an MIZH/MIZS/MIZL layout, so a data.frame's inline and region-backed forms share one grammar, one qualification function (`mizu_interop_attrs_qualify`) and one shape builder per binding. Attribute values are ordinary values, and an attributed value nests where R nests one (a factor column inside a frame's column list, a `dimnames` list with names), but an attr never directly wraps an attr: R has one attribute set per object, and the cursor rejects the double wrap as malformed. The shape whitelist is the *foreign* reader's contract and part of the format-0x01 baseline:

- **factor**: value intv (1-based codes, `INT_MIN` = NA); attrs exactly `levels` (strv, unique, non-NA) and `class` (strv `"factor"`).
- **data.frame**: value a 0x0c list of one or more columns, each an atomic vector tag (0x06–0x0b, 0x0e), a factor attr, or a Date / POSIXct / difftime attr, all of one length n; attrs exactly `names` (strv, unique, non-NA), `class` (strv `"data.frame"`) and `row.names` in one of three forms: intv `c(NA, ±n)` with |n| the column length (automatic row names, the compact form; n = 0 is a legal zero-row frame), an intv of length n (integer row names — a base `df[i, ]` subset), or a strv of length n with no NA (character row names — `mtcars`). A reader building the foreign home — pymizu's `Frame`, mizu's inline attr reader — validates the column shape and the row.names length before it installs the shell; mizu's layout read applies the dict as-is. The **extended frame** (the `MIZU_CAP_LABELED` tier) relaxes the attr set: `class` is a str or strv whose *last* element is `"data.frame"` (prefix classes ride along — a tibble's `c("tbl_df", "tbl", "data.frame")`), the three canonical keys stay as above, and any further keys — the *extras* — name attributes outside the reserved set (`mizu_ix_attr_reserved`: the whole `MIZU_IX_ATTR_*` vocabulary plus `tsp`, which `Rf_setAttrib` validates at apply, so a bad one would longjmp mid-read; a frame never legitimately carries it), a `comment` extra NULL or character (validated at apply likewise), and every extra's value *encodable* (the subset: NULL, atomics, list trees — no environments, closures, language, S4, or external pointers; ALTREP admitted, crossing by value off `*_GET_REGION`). Extras emit in UTF-8 byte order after the canonical keys (deterministic, corpus-pinnable). Inside an extra's value the shape whitelist does not apply: any encodable attribute set emits as `attr(value, dict)` with the dict's keys byte-sorted — the **opaque subtree** (readr's `spec` is a classed list; sf's `agr` a named factor). The R reader applies an opaque subtree as-is behind the special-setter pre-check (`names` length, `dim` product, `dimnames` shape — a corrupt one is the no-home error, never a setter's longjmp); pymizu never decodes one: it stashes the class vector (decoded, for introspection) and each extra's raw byte span (`mizu_ix_skip`) and splices both verbatim on a pristine echo, dropping the stash on any rebuild — so Python relays metadata it cannot home without ever mislabeling reshaped data.
- **named vector**: value an atomic vector tag (0x06–0x0a, 0x0e — as the dim array, character has no ndarray home); attrs exactly `names` (strv of the value's length, NA elements = `NA_character_`). The Python home is a `DimArray` — an ndarray subclass carrying the labels (below). A named integer64's class rides the 0x0e value tag as everywhere, so its dict is `{names}` alone. A named *classed* vector (a named factor, Date, POSIXct) is not this shape — it admits attrs exactly `{names}` — and keeps declining.
- **dim array**: value an atomic vector tag (0x06–0x0a, 0x0e — every atomic tag but strv: numpy has no string-array home that round-trips, so a character matrix declines); attrs exactly `dim` (intv, no NA, every element ≥ 0, product == the value's element count). An integer64 array carries `class` too, which the 0x0e value tag consumes as everywhere, so its dict is still `{dim}` alone. R's column-major order and numpy's Fortran order name one byte layout — the first index varies fastest in both — so the shape is metadata only: the Python home is an ndarray of the value's dtype over the same bytes with `order="F"` (over a received region a stride view, zero copy), and the R home applies `dim` to the vector. A numpy C-order array writes it too, gathering the transpose inside the one stage copy (no temp buffer), and reads back F-order — the documented relay shift. The vector reads per its tag's own rule (the lglv/intv fused scans above) before the reshape. The `MIZU_CAP_LABELED` tier adds the labels: attrs exactly `dim` + `dimnames`, or `dim` + `dimnames` + `class` (str `"table"`, length-1 — R's `table` class, `MIZU_IX_CLASS_TABLE`; an integer64 table's own class still rides 0x0e, so its dict is `{dim, dimnames, class}` with `class` the table mark alone). The `dimnames` value has two forms: **unnamed axes** — a 0x0c list of one entry per axis, each nil (an unnamed axis) or a strv of that axis's extent (NA elements = `NA_character_`); **named axes** — that list itself attributed with exactly `names` (the `names(dimnames(x))`, a strv of exactly `length(dim)` elements; `""` and duplicates are legal — `table(x)` writes `""` for an unnamed argument, and `table(a, b)` on two expressions writes `c("", "")`). The 0x0d dict form cannot carry named axes: keys are unique, and repeated axis names are legal data. The names-attributed list is admitted *only* as the `dimnames` value — a named list anywhere else stays the 0x0d dict. The Python home is the `DimArray` with `dimnames` (and `axis_names`, `r_class = "table"`) attached.
- **Date**: value realv — days since 1970-01-01, NA the `NA_real_` payload, every value integral (a fractional Date declines; the check fuses into the size pass). Attrs exactly `class` (strv `"Date"`). The Python home is `datetime64[D]` — int64 day counts, NaT for NA, whole days exact both ways; Arrow `date32` is the same count in int32, so a `Frame` column exports as `date32` and a polars or pyarrow Date column writes the shape. A stdlib `datetime.date` scalar homes as `Date(1)`: temporals have no scalar tag, so length-1 keeps the attr form (the 0x0e precedent).
- **POSIXct**: value realv — seconds since the epoch, NA the `NA_real_` payload. Attrs exactly `class` (strv `["POSIXct", "POSIXt"]`) plus an optional `tzone` (strv of length 1, non-NA). The instants are UTC whatever the zone; `tzone` is display metadata, and `""` and absent are one value (naive). The Python home is naive `datetime64[us]` UTC — microsecond is the unit where Python datetimes, Arrow's default timestamp and an R double at the current epoch meet; sub-μs fractions round (documented; μs-integral values relay bitwise). NA ↔ NaT, and a genuine instant of exactly INT64_MIN μs reads as NA (the int64 sentinel collision, documented). A named zone survives only as `Frame` column metadata (Arrow `timestamp[us, tz]`), so a standalone named-zone vector arrives naive and its relay returns `tzone = "UTC"` — the factor standalone/in-frame precedent, values exact, display normalized. A naive `datetime64` writes `tzone = "UTC"`; an aware `datetime.datetime` scalar writes its zoneinfo key, so aware scalars relay exactly. A `timestamp[ns]` column is the carve-out: for a `MIZU_CAP_MIZL_TSN` peer the frame writer stages it verbatim — an i64v of counts, NaT the `INT64_MIN` sentinel, the dict gaining `tscale` = `"ns"` (the Arrow frame writer subsection).
- **difftime**: value realv — durations in the declared units, NA the `NA_real_` payload. Attrs exactly `class` (strv `"difftime"`) and `units` (strv of length 1, one of `"secs"`, `"mins"`, `"hours"`, `"days"`, `"weeks"` — R's five; any other declines). The wire carries no precision choice: the value crosses in its declared units and each reader homes it — the Python home is `timedelta64[us]` (microsecond, the POSIXct precedent: where Python timedeltas, Arrow's `duration[us]` and an R double meet), NaT for NA, the five units converting to seconds by value (Arrow `duration` needs no day or week unit — those are 86400- and 604800-second counts on the wire). A relay normalizes the units to seconds — values exact, units display metadata (the `tzone` precedent). numpy `timedelta64` of any non-calendar unit writes the shape (counts to seconds doubles — a double second carries ns resolution to ~104 days), as do the scalar forms (`np.timedelta64`, stdlib `datetime.timedelta`): temporals have no scalar tag, so length-1 keeps the attr form. `[Y]`/`[M]` decline as calendar-dependent, finer-than-ns units as sub-resolution. A `timedelta64` numpy scalar exports its bytes as uint8 (the buffer protocol has no datetime), so the stage gate keeps it off the raw tier — the byte view is not the value (datetime64 scalars the same). The `tscale` carve-out of the POSIXct row applies alike: a `duration[ns]` column crosses verbatim for a `MIZU_CAP_MIZL_TSN` peer.

Any other attribute set has no foreign home: the writer declines it at send on foreign handles (the qualification rules below), and a foreign-home reader meeting one anyway (pymizu's, or mizu's inline attr reader) declines informatively, naming the class and the attribute names. An R reader applies any dict, so on a same-language handle every *encodable* attribute set takes the `'I'` layout blob — the whitelist gates foreign handles only. A later shape is a whitelist entry plus a builder entry, and takes a capability bit like a later tag — never a new tag. The labeled shapes share one bit (`MIZU_CAP_LABELED`) because they land together in both bindings: a writer meeting one for a peer without the bit declines at send naming the capability — metadata is data, never silently stripped — and the gate stands at *every* emission site: the inline writer and each zero-copy filter. A reader without the bit that meets a labeled dict anyway (a version-skewed relay) fails on its exact-set guards, loud, never a silent drop.

The Python home of the data.frame shape is `pymizu.Frame`: column names, a row count, and per-column buffers.
- It exports `__arrow_c_stream__` from pymizu's own C, with no Arrow import. Consumers take it in one line: `pl.from_arrow(f)`, `pa.table(f)`, or `pd.DataFrame.from_arrow(f)` (pandas ≥ 3).
- Validity bitmaps are built lazily at the first export from the in-band NA sentinels (the layout attribute blob's rules, which reuse this machinery). Factor columns export as dictionary columns (0-based indices plus a bitmap), string columns as utf8, Date columns as `date32`, POSIXct columns as `timestamp[us]` (with the `tzone` name as the Arrow timezone when the column metadata carries one), difftime columns as `duration[us]`, and logical columns as Arrow `bool` (bit-packed values plus a validity bitmap for `INT_MIN`) — always, with or without an NA present, so the Arrow type of a logical never depends on its content or its tier. A complex column has no Arrow type: the export raises, naming the column, while `to_dict()` still carries it as complex128.
- Column buffers are C-owned and refcounted between the `Frame` and its exports, so the release callback stays pure C.
- `to_dict()` needs no Arrow library. Numeric columns become numpy arrays (memoryviews without numpy), logical columns follow the lglv rule (`bool_` when NA-free, else int32), Date and POSIXct columns become `datetime64[D]` and naive `datetime64[us]` arrays, difftime columns `timedelta64[us]`, and string and factor columns become `list[str|None]`.
- Row names ride along as `Frame.row_names`: `None` for automatic row names, else a `list[str]` or an int32 array. They are metadata only — no Arrow export or `to_dict()` carries them, since Arrow has no row labels — and the `Frame`'s own export writes them back, so R → Python → R returns them. An Arrow consumer (polars, pyarrow, pandas) drops them at import.
- POSIXct columns keep the `tzone` name as per-column metadata on the `Frame` — the `Frame.row_names` discipline: the `Frame`'s own export writes it back, so a frame relay restores the zone name; Arrow's `timestamp[us, tz]` carries it to consumers, and `to_dict()` stays naive.
- A `Frame` is picklable, so same-language handles carry it. On a foreign handle it writes the frame shape again through its own export.
- An extended frame's metadata rides the `Frame` opaquely: the class vector decoded (a `tuple[str]`, for introspection) and each extra attribute as its raw stream span, stashed at read and spliced verbatim at write — Python never decodes an extra, so there is no length-1-versus-scalar, int32-versus-int64, or nested-classed-value fidelity surface, and the opaque subtree needs no Python home. The **Frame immutability invariant**: the stash rides a pristine received `Frame` only — `from_arrow`, pickling, and every rebuild drop it, and any future `Frame` mutator must clear it first (a grouped_df's `groups` extra is encodable and would mislabel reshaped rows). A rebuilt frame arrives in R as a plain data.frame: honest loss, never wrong labels.

It is the same type the zero-copy tier returns (region-backed). So the Python type of a data.frame depends on neither its size nor the reader's installed libraries. A labeled vector or array homes as `pymizu.DimArray`, an ndarray subclass carrying `names` / `dimnames` / `axis_names` / `r_class`, with the same rule: the labels attach at read and validate at write, and numpy operations drop them (`__array_finalize__` never propagates), so a relabeled or reshaped array never inherits labels its data no longer matches.

Scalar versus vector (pinned, because R has no scalars):
- R writes a length-1, attribute-free logical, integer, double, complex or character value as the scalar tag (0x01/0x02/0x03/0x10/0x04). Any other length writes the vector tag, and R reads both forms to the same R value.
- A class-only integer64 writes 0x0e at every length. If it wrote 0x02 at length 1, a relay would read it back as `integer(1)` and its NA as `NA_integer_`. With 0x0e Python gets a length-1 int64 array, the same home as the top-level RAWVEC INT64 frame.
- **On foreign handles the rule also applies at top level.** A length-1 attribute-free logical, integer, double or complex stages as the `'I'` scalar tag instead of RAWVEC. STR1 already gives Python a `str` at top level, so R scalars now reach Python as scalars at top level and nested alike. Raw and integer64 keep RAWVEC.
- The relay consequences are container shifts. A Python scalar relayed through R returns as a Python scalar. A Python length-1 int32, float64, complex128 or `bool_` array relayed through R also returns as a Python scalar; a length-1 int64 or uint8 array returns as an array, since 0x0e and rawv have no scalar form. Both are documented in the dtype matrix, and the corpus pins them with split `dec`/`enc` rows.

Writer qualification rules (fidelity-preserving, fast to check):

- **R**: `TYPEOF` + `ANY_ATTRIB(x)` gate first (O(1)). Any attribute except dict-rule names on a VECSXP routes to the attr qualification, and pairlists (LISTSXP) decline outright — no VECSXP coercion. The attr qualification is one function, `mizu_interop_attrs_qualify(x)`, and the zero-copy stage calls the same one to decide whether a layout's attribute blob can be `'I'`. It answers two questions: *encodable* — the attribute names are unique, non-NA and writable as UTF-8 (the dict-key rules) and every attribute value is in the subset; and *whitelisted* — the (class, attribute set) pair is a shape the foreign reader homes:
  - **A plain factor**: class exactly `"factor"`, attributes exactly `levels` and `class`, and levels unique, non-NA, and writable as UTF-8 under the string rules below.
  - **A plain data.frame**:
    - class exactly `"data.frame"`;
    - attributes exactly `names`, `class` and `row.names`;
    - column names under the dict-key rules (non-NA, writable as UTF-8, unique after translation);
    - row.names an integer or character vector of length n with no NA. The C API hands the writer only the expanded form: on R ≥ 4.6 `R_getAttributes` returns automatic row names as `1:n` (verified on 4.6.1), as `getAttrib` does. So the writer detects automatic row names by value — an integer vector equal to `1:n`, one O(n) pass over `INTEGER_GET_REGION`; on R < 4.6 the raw `ATTRIB` pair `c(NA, ±n)` answers in O(1) — and writes `intv[na, −n]`. R itself compacts any integer `1:n` it is given for n > 2, so the two are one value. Any other integer vector writes as intv, a character vector as strv under the string rules below. A zero-row frame's attribute is `integer(0)` and writes `intv[na, 0]`, so automatic row names have one wire form at every n; R applies `c(NA, 0L)` as read (`identical()` to the native `integer(0)` frame — verified);
    - one or more columns, each an attribute-free atomic vector (raw columns included, as rawv), a class-only integer64 (0x0e), a plain factor, a Date, a POSIXct, or a difftime.
  - **An extended data.frame** (the `MIZU_CAP_LABELED` tier): the plain frame's column and row.names rules unchanged; class a STRSXP whose last element is `"data.frame"` (prefix classes ride along — tibble, user classes); attributes ⊇ `names`, `class`, `row.names`; every further key outside the reserved set (`mizu_ix_attr_reserved` — the vocabulary plus `tsp`), a `comment` NULL or character, and every extra's value *encodable*. Extras emit byte-sorted after the canonical keys; an attributed extra's value emits as `attr(value, dict)` with byte-sorted keys (the opaque subtree — the whitelist does not apply inside an extra).
  - **A named vector** (the `MIZU_CAP_LABELED` tier): attributes exactly `names`; the value any atomic vector but character; `names` a STRSXP of `length(x)`, NA allowed. A named integer64 qualifies too — its class rides the 0x0e tag, so its dict is `{names}` alone.
  - **A plain dim array**: no class; attributes exactly `dim`; `dim` an integer vector of length ≥ 1 with no NA, every element ≥ 0, and product equal to `length(x)` (a matrix is the length-2 case; a length-1 `dim` qualifies, its documented relay shift a plain vector — numpy cannot tell it from one). The value is any atomic vector but character (strv has no ndarray home), integer64 included: its `class` rides the 0x0e value tag as everywhere, so the dict stays `{dim}`.
  - **A dim array with dimnames** (the `MIZU_CAP_LABELED` tier): the plain dim rules, plus attributes exactly `dim`, `dimnames`, and optionally `class` exactly `"table"`; `dimnames` a VECSXP of `length(dim)` entries, each NULL or a STRSXP of that axis's extent (NA allowed), itself attribute-free or carrying exactly `names` (a STRSXP of exactly `length(dim)` — `""` and duplicates legal).
  - **A Date**: class exactly `"Date"`, attributes exactly `class`, every value integral — fractional days have no `datetime64[D]` home and decline, the check fused into the size pass.
  - **A POSIXct**: class exactly `c("POSIXct", "POSIXt")`, attributes exactly `class` and optionally `tzone` — `tzone` a length-1 non-NA string, with `""` and absent the one naive value.
  - **A difftime**: class exactly `"difftime"`, attributes exactly `class` and `units` — `units` a length-1 non-NA string in R's five (`secs`, `mins`, `hours`, `days`, `weeks`). The value crosses as-is (the reader applies the unit), so no conversion runs on the R write.

    The reader applies the row.names it reads (`Rf_setAttrib` compacts the automatic form). `identical()` compares row.names through `getAttrib`'s expansion, so either sign round-trips.
  - **A class-only integer64** is not an attr: it writes 0x0e at any length — class exactly `"integer64"`, no other attributes (the `mizu_raw_type()` gate).

  The inline attr writer runs on foreign handles only, so it requires both answers. These decline: a named *classed* vector (a named factor, Date or POSIXct — the `names` shape admits attributes exactly `{names}`), a dim array of character, an ordered factor, a zero-column frame, a data.table (its `.internal.selfref` external pointer fails the encodable gate — the frame's extended form never sees it), a groupedData frame (a formula attribute likewise), a POSIXlt, a difftime with a units outside R's five, and a fractional Date. A named atomic vector, a named integer64, a dim array with `dimnames`, a `table`, and a frame subclass whose extra attributes are all encodable (every tibble) now cross, on the `MIZU_CAP_LABELED` tier.

  The interop writer runs only on foreign handles, where ALTREP values cross by value instead of declining. This includes a top-level view whose layout the peer cannot wrap, which the zero-copy-tier filter routes here. A nested mizu view crosses by value too, and the wire-hooks path stays the same-language form. Past the zero-copy floor a top-level ALTREP atomic does not come here: the zero-copy-tier filter admits it to MIZH, and the layout write copies through `*_GET_REGION` (the layout leaf rule). That costs one copy on the sender, and the reader gets a zero-copy view instead of a parse and a copy.

  - **Size pass.** Atomics need only `XLENGTH`, so it touches no data. ALTREP strings are walked with `STRING_ELT`, which the size pass does anyway to count bytes.
  - **Write pass.** Non-resident atomics are copied through `*_GET_REGION` in bounded chunks. Compact sequences compute their values in place, so `1:1e9` is never expanded on the sender. A mizu view with a readable data pointer is copied straight off the shared pages.

  A class whose region method raises unwinds inside stage, which transactional staging already covers; the bindings' suites verify the spill-path checkout's rollback. This is value-exact, because `identical()` never compares representation. Same-language handles never reach the interop writer, so R→R keeps R_Serialize's compact representations. A deferred-string ALTREP may cache elements on the sender through `STRING_ELT`: its values are untouched, but its representation is not, so "the sender's object is untouched" is claimed for atomics only.

  Strings are written as UTF-8:
  - ASCII and CE_UTF8 as-is;
  - unmarked native strings as-is on the UTF-8-native supported platforms, after a validity check (invalid UTF-8 declines);
  - CE_LATIN1 translated through `Rf_translateCharUTF8`, which is total for latin1. The reader constructs CE_UTF8, and `identical()` is TRUE: it compares strings through `Seql`, which translates across encodings (verified for strings, list names, and factor levels).

  CE_BYTES declines. The rule applies wherever a string crosses: dict keys, factor levels, strv elements, 0x04 scalars.
- **Python**: exact-type checks as today (subclasses decline; they keep pickle on same-language handles); int past int64 declines; lone-surrogate str declines; exact-type `bytes` writes 0x05 (nested — a top-level `bytes` keeps the raw tiers); exact-type `complex` writes 0x10; an exact-type `tuple` writes 0x0c, the list tag — no reader materializes a tuple (Python→Python never writes `'I'`; R's home is `list` either way), so it reads back as a `list`, the documented relay shift. A multi-dimensional numpy array — the buffer gate rejects `ndim > 1` (`wire_type_of` :291, `stage_convert_buffer` :776), so today it has pickle only — writes the dim shape when its dtype maps, the identity and lossless conversion rows applied elementwise (uint64's lossy row stays top-level-only, and a dim array is top-level). An F-order array memcpy's; a C-order array gathers its transpose; a strided or negatively-strided array normalizes — each inside the one stage copy, no temp buffer. numpy `datetime64` leaves the unmapped-dtype decline for the date and time units: `datetime64[D]` writes the Date shape, `[W]` converts to days (×7, exact), and the time units `h`/`m`/`s`/`ms`/`us`/`ns` write the POSIXct shape — counts to epoch-seconds doubles, sub-μs resolution rounding, NaT to the NA payload. `[Y]`/`[M]` decline as calendar-dependent, finer units as sub-resolution, and stdlib `datetime.time` has no home. numpy `timedelta64` of a non-calendar unit and stdlib `datetime.timedelta` write the difftime shape — counts to seconds doubles, NaT to the NA payload. stdlib `datetime.date` and `datetime.datetime` write the length-1 shapes — an aware datetime's zoneinfo key as `tzone`, a naive one `"UTC"`. Arrow `date32`, `timestamp[unit, tz]` and `duration[unit]` columns write the Date / POSIXct / difftime shapes in the stream front-end and the frame writer (the Arrow frame writer subsection; the zone name riding the column's `Frame` metadata); `date64`, `time32`/`time64` and `interval` keep declining. Frames write the attr frame shape only through `__arrow_c_stream__` (below); there is no pandas-specific writer.

  **Representation-shifting admissions** (value-exact; like all interop writing, foreign handles only):
  - numpy scalars of mapped dtypes write their scalar tag. Nested `np.float64`, `np.int64` and `np.bool_` are not exact `float`, `int` or `bool`, so without this a per-element `np.mean(x)` result in a dict would decline.
  - nested buffer leaves and frame columns take every lossless conversion-pass row: bool → lglv, narrow ints → intv, uint32 and float32 → realv, complex64 → cplxv. uint64's lossy-past-2^53 conversion declines when nested.
  - any object exporting `__arrow_c_stream__` writes the attr frame shape — `attr(list[columns], {names, class = "data.frame", row.names = c(NA, -n)})`, R's attribute vocabulary, which is also what the frame writer emits as a root blob by default (the Arrow frame writer subsection), so one frame-shape emitter serves both tiers. This covers a pyarrow Table, a polars DataFrame, a duckdb relation, a received `pymizu.Frame`, or a pandas DataFrame (pandas ≥ 2.2), found by dunder probe with no import. Details:
    - all record batches are read and concatenated per column in the write pass, which copies anyway;
    - each column is a `cvt_for_arrow` dtype under the same lossless rule;
    - Arrow strings write 0x0b, null → len −1: utf8 `u` and large_utf8 `U` (offsets walked), and string_view `vu` (views walked: inline at ≤ 12 bytes, else a data-buffer reference). string_view is the only form polars exports, and polars ignores `requested_schema`, so without it every polars frame with a string column would decline;
    - a dictionary-encoded column (unique non-null utf8, large_utf8 or string_view dictionary — a polars Categorical exports `dictionary<string_view, uint32>`) writes the factor shape — `attr(intv codes, {levels, class = "factor"})`. Indices of any integer width are widened to 1-based int32 (pandas exports int8, polars uint32), with null → INT_MIN. A dictionary with `ARROW_FLAG_DICTIONARY_ORDERED` set declines, as the factor shape has no ordered bit in format 0x01 — this includes every polars Enum, which exports ordered (remedy: cast to `pl.Categorical`);
    - validity bitmaps become the column tag's NA sentinel (the masked forms of `mizu_cvt_convert`, the Arrow frame writer subsection);
    - field names must be unique;
    - struct/list columns, decimals, `date64`, `time32`/`time64` and `interval` decline (`date32` and `timestamp[unit, tz]` write the Date / POSIXct shapes, above; a `duration[unit]` column writes the difftime shape);
    - schema metadata is ignored. pandas attaches `b'pandas'` to every export, so a metadata rule would decline every pandas frame.

    pandas frames need no pandas-specific code, because pandas' export (through `pa.Table.from_pandas`) already maps what a native writer would have to probe. Three consequences, all documented:
    - **pyarrow is required.** pandas does not depend on pyarrow, and its export raises `ImportError` without it. The size pass calls the export, so any exception from it (the missing pyarrow, or pyarrow rejecting a mixed-object column) becomes a `DeclinedError` that names the cause. The pyarrow case names the install.
    - **pyarrow decides what happens to row labels.** A non-range index arrives as a column (`__index_level_0__`, or the index's name). A RangeIndex, even a named or offset one, is dropped. Documented as "pandas row labels do not cross"; R row names reach Python only as `Frame.row_names` metadata, which no Arrow consumer imports.
    - **Column labels become strings.** pyarrow stringifies non-str pandas column labels.

- **Both**: a declined value never partially writes. The size pass validates everything, so the write pass cannot *decline* (the existing `codec_size`/`codec_put` discipline in both bindings). The one mid-write failure is an ALTREP class whose region method raises (the R rules above), and transactional staging covers it.

The byte grammar has one executable form, vendored with the core: `src/interop.c`'s validating pull cursor (`mizu_ix_open` / `mizu_ix_next` / `mizu_ix_end`) owns bounds, the depth cap, UTF-8 validity, the container arity accounting, the attr-frame positions, the top-level-only err rule, the reserved-flag rejections, and the informative unknown-tag / unknown-version / unknown-kind declines, while the `mizu_ix_put_*` dual-form helpers are the one emit half every binding's two-pass walk writes through — a binding keeps only a builder (native allocation per item) and a value walk. The golden corpus (`tests/interop/cases.txt` → `corpus.txt`, generated by the spec-derived stdlib reference `tools/interop_corpus.py` and certified by `tests/unit/test_interop.c` plus the `tests/fuzz/` cursor harness) pins the grammar once here, so each binding's conformance test can assert its own builder and walk against the fixtures instead of against a peer's writer. It is a library module the bindings call, not a transport path: the transport still never interprets a slot payload on its own.

### The byte-shape helper registry

The morsel.c / stage_raw.c / interop-cursor precedent, applied to the rest of the 'I' machinery that operates purely on byte spans and cursor items: the core owns protocol bytes; bindings own values.
This subsection is the specification of record for the helpers the core absorbs so that no byte-level rule keeps a second implementation free to drift across bindings; it lands as the ext surface it names, and the bindings adopt in that release (each deleting its copies in the adopting commit).
No binding object crosses the boundary — every helper takes spans, counts and cursor items, never a SEXP or a PyObject.
A span is a (pointer, byte count) pair; the pointer shall reference at least the stated count.
The bindings keep: the value walks, the attribute *qualification* (which attribute sets are encodable or whitelisted — it walks native objects), the error idioms (longjmp, `PyErr`), and the err stream's *mode* logic (which spans a condition contributes: mizu's remote-relay fields versus its flatten, the map runner's element-index extraction); the core takes resolved spans.
The wire does not change under any item here: byte-identity is pinned by the golden corpus, `MIZU_ABI_VERSION` is untouched, and the 'I' version byte stays 0x01.

Requirement keywords in this subsection take the ISO/IEC reading: **shall** an absolute requirement, **shall not** an absolute prohibition, **should** a recommendation departed from only for recorded cause, **may** an allowed option.
A binding conforms to the 'I' format only when every shall here and in the format subsection holds.

| Helper | Form | Single-sources |
|---|---|---|
| `mizu_ix_utf8_valid` | dual-form | the four hand-copied RFC 3629 validators (the cursor's own included) |
| `mizu_ix_write_err`, with `MIZU_IX_ERR_OVERHEAD` / `MIZU_IX_ERR_TYPE_SHARE` | dual-form + macros | the bounded (type, message, detail, index) err framer, algorithm-identical in the two bindings |
| `mizu_ixt_open`, `mizu_ixt_want_code`, `mizu_ixt_want_list`, `mizu_ixt_want_dict` | exported only | the task-stream decode shim, per-binding copies with byte-identical error text |
| `mizu_ix_tag_of` | dual-form | the triplicated wire-type → vector-tag switch (`mizu_ix_put_vec`'s included) |
| `MIZU_IX_ATTR_*` / `MIZU_IX_CLASS_*` / `MIZU_IX_UNIT_*` | macros | the shape whitelist's attr keys, class strings and difftime units, hard-coded in two languages |

The form column carries one discipline, stated once: *dual-form* is the `mizu_ix_put_*` pattern — a `static inline` in `mizu_ext.h` plus a same-named exported symbol in `src/ext.c`, the body single-sourced in the header — and the classifier is self-containment: pure byte math over header-defined constants is dual-form (it exists for the hot paths, but a cold helper that is self-contained — the err framer — takes the same form); *exported only* is for code that wraps the cursor or records TLS errors, the cursor's own form; *macros* are compile-time constants and export no symbols.
A helper that records TLS errors is never an inline — `mizu_err_record_tls` is internal, and the installed header references installed symbols only — and cold protocol code buys nothing from inlining, so the shim keeps the cursor's exported-only form.
Every declaration and macro here lives in `mizu_ext.h`'s interchange stream ('I') section, beside the cursor and the emit helpers.

**The UTF-8 validator.**

```c
int mizu_ix_utf8_valid(const void *s, size_t n);
```

Returns 1 when the `n` bytes at `s` are a well-formed UTF-8 byte sequence per RFC 3629 (the Unicode Standard's well-formed byte sequence table names the same set): shortest-form encodings only — overlong forms rejected — no UTF-16 surrogate code points U+D800–U+DFFF, no code point past U+10FFFF, no stray or truncated continuation bytes; 0 otherwise.
The empty span is valid.
`s` shall be NULL only when `n` is 0.
A pure predicate: O(n), no allocation, no error record.
The cursor's own string checks single-source through it, so the stream's UTF-8 rule and the bindings' string rules have one implementation.

**The err framer.**

```c
#define MIZU_IX_ERR_OVERHEAD 25u    /* magic + version + tag + flags + index + three counted lengths */
#define MIZU_IX_ERR_TYPE_SHARE 128u /* the type span's share cap */

size_t mizu_ix_write_err(unsigned char *dst, uint32_t inline_max,
                         const void *type, size_t type_n,
                         const void *msg, size_t msg_n,
                         const void *detail, size_t detail_n,
                         int has_index, uint64_t index);
```

Composes a complete 'I' stream — the two-byte header, then the 0x11 err item — implementing the format section's bounded-and-cannot-fail writer.
The caller passes resolved spans; the budget algorithm runs in size_t arithmetic:

```
avail = inline_max > MIZU_IX_ERR_OVERHEAD ? inline_max - MIZU_IX_ERR_OVERHEAD : 0
tn = floor(type,   type_n,   min(avail, MIZU_IX_ERR_TYPE_SHARE))
mn = floor(msg,    msg_n,    min(inline_max / 2, avail - tn))
dn = floor(detail, detail_n, avail - tn - mn)
```

`floor(s, n, share)` is `n` when `n <= share`, else the largest `len <= share` that does not split a multi-byte sequence: `while (len > 0 && (s[len] & 0xC0) == 0x80) len--`.
The three spans shall be valid UTF-8 — per `mizu_ix_utf8_valid` or the binding's source-encoding guarantee: the floor preserves validity under that precondition and is merely bounded without it; the framer never validates.
The item is emitted with flags bit 0 = `has_index`, the `index` u64 present only when set, then the three truncated spans as the bare counted strings; the framer builds on `mizu_ix_put_err`, so the budget algorithm above is the single thing it adds, and it lives exactly here.
The return is the stream size; a NULL `dst` is the size query, the `mizu_ix_put_*` convention.
`inline_max` shall be at least `MIZU_IX_ERR_OVERHEAD`; the return is then at most `inline_max` — the fits-the-slot-by-construction guarantee, so the caller stamps INLINE with the keeperless claim and the writer cannot fail: no allocation, no error path, no TLS record.
The u32 width of `inline_max` is the slot inline budget's own, and it bounds every truncated length into the wire's u32 count fields.
The constants live in `mizu_ext.h` as the wire authority for the budget.

**The task-stream decode shim.**

```c
mizu_status mizu_ixt_open(mizu_ix *cur, const void *buf, size_t len,
                          mizu_ix_item *item, int max_kind);
mizu_status mizu_ixt_want_code(mizu_ix *cur, mizu_ix_item *item);
mizu_status mizu_ixt_want_list(mizu_ix *cur, mizu_ix_item *item);
mizu_status mizu_ixt_want_dict(mizu_ix *cur, mizu_ix_item *item);
```

`mizu_ixt_open` serves every task-stream decode — the exec decode, the runner decode, and the inspection hook's (mizu's `mizu_interop_read_task_call`, pymizu's `pymizu_ix_read_task_components`; a test and inspection path, not a worker).
The `want_*` checks serve the decodes that read call fields — exec, inspection hook, and the map descriptor's spec half, which applies them after its own framing checks: the field tags are the builder's check, not the cursor's (the format section), and the shim is that check, once.
`mizu_ixt_open` opens the cursor, pulls the first item, and requires the TASK header with `task_kind <= max_kind`, the decode site's ceiling within the cursor's registry — a runner stream at the exec decode fails informatively instead of misparsing.
On success `*item` is the header item and the submitter identity rides `item->u64[0]`: the binding stashes it by its own discipline ahead of the field reads (mizu's `mizu_curpool_ident`, pymizu's `ident_out`), so the shim takes no stash parameter and touches no binding state.
The `want_*` checks pull the next item and require, respectively, a non-NA STR1 (the 0x04 code string), a LIST begin (the positional field), a DICT begin (the named field).
Every failure is `MIZU_ERR` with the text recorded in the thread-local slot (the cursor's own discipline), so the texts are byte-identical across bindings by construction rather than by copying; each binding surfaces the slot with its own prefix, as both already do for cursor errors.
A failure from the cursor itself carries the cursor's own text; the shim adds exactly five, and they are normative:

| Site | Text |
|---|---|
| first item not TASK | `malformed task stream: no task tag` |
| `task_kind > max_kind` | `unsupported task kind 0x%02X` (the kind byte) |
| `want_code` | `malformed task stream: the code field is not a string` |
| `want_list` | `malformed task stream: the positional field is not a list` |
| `want_dict` | `malformed task stream: the named field is not a dict` |

The `0x%02X` text covers kinds inside the cursor's registry but above the site's ceiling; kinds past the registry stay the cursor's own `unknown task kind 0x%02X — the peer uses a newer format` decline.
Binding-side, and explicitly out of the shim: the item pull itself (that is `mizu_ix_next` — an alias would add a symbol, not a check), the target-byte misroute guard (the worker's language is binding state), the runner stream's own field texts (`malformed runner stream: …`), the qualified-name resolution and per-kind call construction, the dict-key decode and duplicate rejection (builder work — the cursor holds no key set — whose texts differ by binding today), and the map descriptor's `list[task, x]` framing checks, whose texts name the descriptor.

**The vector-tag table.**

```c
int mizu_ix_tag_of(int wire_type);
```

The wire-type → vector-tag mapping, one row per atomic wire type: LGL → 0x06, INT → 0x07, REAL → 0x08, CPLX → 0x09, RAW → 0x0a, INT64 → 0x0e.
Any other input returns 0 — `MIZU_IX_TAG_NIL`, never a vector tag, so the failure is distinguishable from every mapping.
`mizu_ix_put_vec`'s internal switch single-sources through it, keeping the emit and the table in one place.

**The attribute vocabulary.**

The shape whitelist's wire strings, as macros — compile-time constants need no exported twin, and lengths ride `sizeof - 1` at the call sites that pass one:

```c
#define MIZU_IX_ATTR_NAMES    "names"
#define MIZU_IX_ATTR_LEVELS   "levels"
#define MIZU_IX_ATTR_DIM      "dim"
#define MIZU_IX_ATTR_CLASS    "class"
#define MIZU_IX_ATTR_UNITS    "units"
#define MIZU_IX_ATTR_ROWNAMES "row.names"
#define MIZU_IX_ATTR_TZONE    "tzone"
#define MIZU_IX_ATTR_TSCALE   "tscale"

#define MIZU_IX_CLASS_FACTOR    "factor"
#define MIZU_IX_CLASS_DATE      "Date"
#define MIZU_IX_CLASS_POSIXCT   "POSIXct"
#define MIZU_IX_CLASS_POSIXT    "POSIXt"
#define MIZU_IX_CLASS_DIFFTIME  "difftime"
#define MIZU_IX_CLASS_DATAFRAME "data.frame"

#define MIZU_IX_UNIT_SECS  "secs"
#define MIZU_IX_UNIT_MINS  "mins"
#define MIZU_IX_UNIT_HOURS "hours"
#define MIZU_IX_UNIT_DAYS  "days"
#define MIZU_IX_UNIT_WEEKS "weeks"
```

`mizu_ext.h` is the wire authority for these strings: they are the format-0x01 baseline's vocabulary, so the set is append-only under the language-value rule — a rename is a wire change and never happens; a later whitelist shape adds rows (and takes a capability bit, per the format section).
The vocabulary is exactly the whitelist's: `"ordered"` never crosses (the writer declines it) and `"integer64"` rides the 0x0e value tag, so neither takes a constant; timezone names are an open value space — any zoneinfo key, pymizu's naive-datetime fallback `"UTC"` included — and take none either. `tscale` carries the one value `"ns"` — the verbatim temporal form's scale label, allocated with `MIZU_CAP_MIZL_TSN` (the Arrow frame writer subsection).
Layout blobs in the whitelist shapes use these same constants; a same-language blob may additionally carry arbitrary attribute names — an open key space the vocabulary does not cover.

**Verification and adoption.**

- `tools/exports.txt` gains seven symbols via `--write` — `mizu_ix_utf8_valid`, `mizu_ix_write_err`, `mizu_ix_tag_of`, and the four `mizu_ixt_*` — no leaks, no missing; the release carrying the helpers records them in CHANGELOG.md.
- The ext tier (`tests/ext_surface.c`) exercises every dual-form helper in both forms (`EXT_PROBE_EXPORTS` proves they agree), and the amalgamation smoke test covers them.
- The unit tier (`tests/unit/test_interop.c`) pins: the validator's edge matrix (overlong forms, surrogates, the U+10FFFF boundary, truncated and stray continuations, the empty span), the framer's budget matrix (each span truncated at and past its share, the UTF-8-boundary step-back, the index flag on and off, `inline_max` at the 25-byte minimum, the NULL-dst size query), the shim's accept and reject rows with the exact TLS texts, and the full tag table plus the unknown-type zero.
- The wire does not change, so the golden corpus passes unmodified — that is the acceptance gate — and `MIZU_ABI_VERSION` stays.
- The framer is emit-only (no fuzz target); the shim's inputs are the cursor's outputs, the byte space `tests/fuzz/fuzz_interop.c` already covers.
- No performance expectation: acceptance is byte-identity, the surface checks, and line-count removal in the bindings.
- Adoption deletes the copies: mizu's `ix_utf8_ok`, `ix_err_floor` and the budget half of `mizu_interop_write_err` (the SEXP mode logic stays), `ix_vec_tag`, and its `ixt_*` (`ixt_call`, name resolution and the keyset stay); pymizu's `ix_utf8_valid` (both copies, `interop.c`'s and `_pymizu.c`'s), the body of `pymizu_ix_write_err` (already the core's shape — adoption is mechanical), `ix_tag_of_wire`, and its `ixt_*`. Both bindings re-vendor. One copy stays by design: mizu's private flatten floor (`condition.c`) truncates the same-language ERR format, not the 'I' err tag.

### The Arrow frame writer

The Arrow C Data Interface → MIZL frame writer (`src/arrow.c`), the morsel.c / stage_raw.c / interop-cursor precedent applied to the frame tier: one implementation both bindings stage through, promoted from pymizu's shmframe.c with the wire bytes pinned by the cross-language suite.
The core owns the protocol bytes — the column classification, the batch vetting, the conversion kernels, the layout write and the attribute emitters — and the bindings keep what touches language objects: the import front-ends that produce the caller-owned Arrow structs, the provenance registry behind the remote-leaf override, and any emitters of their own driven off `mizu_arrow_col_info` (pymizu's inline frame forms).

The ABI structs are the spec's, defined in `mizu_ext.h` under its two include guards — frozen ABI, no headers, no dependency.
The writer borrows them: the schema and the batches pointer array are caller-owned and shall outlive the handle until `mizu_arrow_close`, and the writer reads them only inside the write.

`mizu_arrow_open` classifies every column off the schema (a struct schema) and vets every batch before a byte is staged — buffer counts, offsets, null-count range, column lengths, unnamed or duplicate names, a zero-column frame — and validates the dictionaries: batch 0's levels canonical and stable across batches, every index range-checked.
The column kinds:

- **CVT**: a fixed-width column through its `mizu_cvt_row` (the conversion table below), the identity rows included.
- **STR**: utf8 / large_utf8 / string_view → an MIZS leaf, the producer's UTF-8 taken on Arrow's contract; the catastrophic cases (negative or non-monotonic offsets, a view buffer index past the buffer array, or — for a producer that wrote the spec's sizes array — a view span past its declared size) decline. A view naming the trailing slot proves a no-sizes export (nanoarrow's nested or standalone form appends none) and crosses with its span unbounded.
- **DICT**: a dictionary of strings → a factor: 1-based INT32 codes, null → INT_MIN, the levels blob. An ordered dictionary declines (the factor shape has no ordered bit in format 0x01).
- **DATE / TS / TD**: date32 → a REAL leaf of days + the Date blob; `timestamp[unit, tz]` and `duration[unit]` (units s/m/u/n) → a REAL leaf of seconds + the POSIXct or difftime blob, the zone name riding the POSIXct blob. The ns unit is the `tscale` case below.

The conversion kernels (`mizu_cvt_for_arrow` / `mizu_cvt_convert`) single-source the fixed-width rows every binding previously kept: identity copy, sign- and zero-widening narrow ints to int32, uint32 and uint64 to double (past 2^53 → `NA_real_`), float16 and float32 to double (exact), bool byte- and bit-packed to logical, complex64 to complex128.
The producer's validity bitmap is authoritative (the validity-as-data section below) — copied into the region's validity tail verbatim for a single offset-0 batch, rebuilt honouring the batch offset otherwise — and the data carries the R NA sentinels at null slots, patched in the copy; an Arrow NaN is a value and stays NaN.
A uint8 column's nulls declines (R raw has no NA).
The loss events accumulate as counts (`mizu_arrow_warn`: uint64 past 2^53, a genuine INT32_MIN, a genuine INT64_MIN) which the binding raises after the stage returns, never mid-stage; the sentinel counts run only on the masked walk (a null-free column takes no walk and never counts), and the verbatim rows (nanosecond temporal, uint64) are excluded — a cap-peer reads those lanes exactly.
The write is always the foreign form — bulk CE_UTF8 marks and the validity tail — so the bytes are peer-agnostic: an R reader takes the sentinels and ignores the tail, a Python reader takes the bitmap.

**The verbatim nanosecond form.** A `timestamp[ns]` or `duration[ns]` column for a peer advertising `MIZU_CAP_MIZL_TSN` (bit 5) stages verbatim: an INT64 leaf (the identity i64 row, NaT the `INT64_MIN` sentinel) whose blob gains `tscale` (the 0x04 str scalar `"ns"`) beside `class` + `tzone` or `class` + `units` — the i64 counts' scale label, the non-verbatim blob needing none (its REAL values are always seconds, so `units` is fixed).
Without the bit the same column takes the REAL-seconds form (the 1e-9 rescale; sub-µs resolution rounds, as the whitelist rows document).
One bit, one layout variant — the versioning rule's new-bit-for-a-gated-shape case.
The uint64 column's verbatim form is the same shape: a `'L'` column for a peer advertising `MIZU_CAP_MIZL_U64V` (bit 6) stages as an INT64 leaf whose blob is `{u64 = TRUE}`, else the REAL conversion — the validity-as-data section below is normative for both.

`mizu_arrow_stage` is the one-call stage over the handle seam, valid inside a binding's `stage` hook.
The gate runs first, touching no shared state: the capability conjunction (`MIZU_CAP_MIZL` | `MIZU_CAP_ATTRS`, plus `MIZU_CAP_MIZS` when any column is a string) off the classification and the peer's mask, then the churn check.
Then the reap, the spill checkout, the layout write, the SHM_VEC stamp and the zc retain.
A non-OK return claims nothing, so the binding's copy tiers take the gate's decline and the spill status alike.
The two bindings' previously separate conjunction and churn rules have their one home here.

The overrides re-run the memoized size pass on the next `mizu_arrow_size` / `mizu_arrow_stage` call: `mizu_arrow_remote` marks a column as a directory-tag-33 remote leaf (pymizu's provenance match; one REFHELD OR per distinct referenced region at write, a torn referenced tree leaving the column local); `mizu_arrow_root_blob` replaces the frame attribute dict (pymizu's custom row_names — absent the override the writer emits names + class data.frame + the automatic `c(NA, -n)` row.names); `mizu_arrow_tz` pins a POSIXct leaf's zone name (a same-language sender's naivety-preserving form; the classified default writes `"UTC"` for a naive timestamp).

Wire invariants: a frame is one self-contained MIZL region — the 64-byte header, one 32-byte directory entry per column, the column bodies at 64-byte-aligned offsets, the leaf blobs, the root frame blob, then the validity tail — framed SHM_VEC with the exact used byte count; remote leaves are the only outgoing references, their REFHELD pins the writer's own.
Staging is transactional: the writer is pure C over caller-owned memory until the layout write lands in the region.

### Validity as data

When a MIZH or MIZL header's validity pair is not `{0, 0}`, the section it locates is the **authoritative null record** for the layout: the in-band sentinels (`NA_real_`'s NaN payload, `INT_MIN`, `INT64_MIN`) are the null-slot fill, not the record. `{0, -1}` asserts authoritatively null-free.
The declaration is semantic, not byte-level: the section already exists, so it needs no capability bit — a pre-dating reader simply never looks, and a new reader on an absent section falls back to the sentinel scan, today's behavior in both directions. `MIZU_ABI_VERSION` stays 1.

**The fill invariant.** A writer that emits a section writes the wire type's sentinel at every clear bit.
The bitmap and the sentinels never disagree in the null direction, so a reader that ignores the section (R's value reads) still reads every null as NA; they may disagree only in the other direction — a *set* bit over a sentinel value is a genuine value.

Readers split on whether the boundary has a null concept.
At one (an Arrow export, a Frame export) a carried bitmap applies verbatim, and on an absent section (`{0, 0}`) the reader derives it lazily from the sentinels — one rule for every atomic wire type, REAL included (its test is the payload, not `isnan`: an Arrow NaN is a value and stays valid).
At a boundary without one (R values, numpy) a set valid bit over a sentinel value is a genuine value; where the read walks the bitmap anyway it warns and names the fidelity accessor, but the landing itself is unchanged — the one irreducible collapse, R's data model.

Writers emit the section whenever the source carries a null record (an Arrow validity bitmap, a foreign fused build).
A writer whose data model has no genuine-sentinel concept may leave `{0, 0}`; `{0, -1}` asserts null-free and makes a genuine sentinel a value on every Python read, so the stamp belongs on data the writer knows cannot carry a null.
The `'I'` stream has no validity section — the sentinels are its whole null record, as the tag table's 0x02/0x03 cells document.

**The uint64 verbatim form.** A `'L'` (uint64) frame column for a peer advertising `MIZU_CAP_MIZL_U64V` (bit 6) stages verbatim: an INT64 leaf — the identity i64 lanes, the validity machinery unchanged — whose attrs blob carries a u64 marker key.
Without the bit the same column takes the conversion (double; past 2^53 → `NA_real_`, counted) — one bit, one layout variant, the TSN precedent.
Both bindings advertise the bit, so every current-version pair crosses uint64 frames verbatim; the R landing keeps the column a view (double + NA at materialization, exact at the Arrow export), which removes the sign-flip path and makes advertising safe there.
A verbatim region relayed to a pre-cap reader splits by reader: an old pymizu declines on the unknown attr key (its attributed-column fill rejects an unmatched attrs set); an old mizu/mori applies the dict as-is and reads sign-flipped int64 with a stray attribute.
The degradation is channel-scoped: pool worker joins CAS the whole identity word, so a pool's workers never mix versions — the conversion runs for pre-cap channel peers and mixed-version submitters only. `MIZU_ABI_VERSION` stays 1 under the append-only cap policy.

A dedicated null-mask tier was considered and rejected: it duplicates the section machinery, costs a capability bit, and buys nothing.

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
The typedefs in `include/mizu_ext.h` (the binding-author tier) document the full contracts.
Four of them carry the correctness guarantees:

- Staging is transactional.
  The core mutates no shared state before `stage_fn` returns.
  The core reclaims an arena chunk allocated mid-stage in FIFO order, like any other chunk.
  An uncommitted region checkout or pin rolls back at the next verb entry or at destroy.
  A binding can abandon mid-stage (a longjmp) and leave the handle consistent.
- A failed `read_fn` (a NULL return) consumes nothing — the slot stays for a retry.
  Setting `MIZU_READ_CONSUME` first flips that: the transport consumes exactly as on success (the channel head advance and its batched publication; the pool FREE transition, task-keeper drop, and producer-keeper wake) while the verb still returns `MIZU_ERR`.
  This is the foreign/corrupt-payload contract: an unreadable slot must not wedge the ring behind it.
  The binding carries its specific message in its own state; the core records no generic error for a consumed read.
  The consume decision sits with the binding on a single receive and on a batch's first message only.
  A batch returns every message it consumed: a read failure after the first message ends the batch with `MIZU_OK` and the prefix, the failing slot stays at the head, and the next receive reproduces the failure and decides.
  A failed read may therefore run twice — once inside the batch, once at the next receive — so a decline must be re-runnable and leave shared state untouched (between trusted peers a re-run load may re-fire a partially executed unpickle; acceptable).
  A mid-batch failure records no generic error either: the record would be stale against the `MIZU_OK`-with-prefix return, and the next receive reproduces and records it.
- `exec_fn` must not abandon.
  The binding catches each task condition into the result sink.
  A worker that lets one escape degrades to worker death plus the reaper verdict.
  That is the hard-crash semantics, never the path for an ordinary task error.
- `check` runs at abandon-safe points only, once per spin or park iteration.
  A nonzero return unwinds the verb as `MIZU_ERR` with `MIZU_ERRCAT_INTERRUPTED` and consumes nothing.

A forked child holds a copy of its parent's handles: every verb on one fails `MIZU_ERR`, and the teardown splits along the same line.
`destroy` stays the owning process's verb — it writes cross-process words (the close signal, the rendezvous unlink, the shutdown broadcast, the slot release).
`mizu_channel_abandon` / `mizu_pool_abandon` are the inherited handle's verb: only the child's own copies (the mappings, the fds, the heap) are released, no cross-process word moves, so the parent's handle and the wire state are untouched.

### Staging policy

`mizu_stage_raw` (dual-form in `mizu_ext.h`; the slow path is `stage_raw.c`) is the core-owned raw-tier reservation: the RAWVEC / arena-RAWSPILL / region-RAWSPILL / flat-SHM_VEC cascade both first-party bindings stage by the same rules.
It composes the stager services and is valid only during `stage_fn`.
The churn signal is read at most once per stage, gated behind the zero-copy size gate.
A NULL return hands the object to the binding's serialized tiers: a reservation failure degrades, never errors.
Bare bytes carry no identifier, so the raw tiers pin nothing; the flat SHM_VEC reserve writes the MIZH header before `mizu_stage_retain_zc` stores the producer loan.
Object eligibility (which values are raw) stays binding-side, as do the string and list-tree layouts.

### Map morsel protocol

A binding's parallel map rides one fresh region per map call: a 128-byte header, the descriptor stream, an optional bare-bytes x section, the morsel state, and an optional template output area.
The protocol is core-owned (`morsel.c`, `mizu_morsel_*` in `mizu_ext.h`); the descriptor codec, the x-section element I/O, the batch loop, and the gather stay binding-side.
The one magic is `MIZU_MORSEL_MAGIC`, stamped and checked by the module itself; the descriptor's codec identity rides the descriptor stream's own first byte (R_Serialize, pickle, or `'I'`), which is where each binding's reader dispatches.
A cross-language map is this same region with the `'I'` descriptor form — one stream, `list[task, x | nil]`: the f spec nested as a kind 0/1 task tag, the list-x values as a bare 0x0c list (nil when the raw section carries them, a 0x13 ref leaf when x is a received view — the descriptor shares the value grammar, so a view x crosses by reference and the worker's map context holds the resolved view between morsels) — and kind-2 runner tasks in place of private runner frames.
The worker's exec hook dispatches kind 2 to its own binding's native runner loop, so the runner is always same-language as the worker.
The runner's *result* shape stays binding-private, and a foreign collect normalizes: mizu's runners publish `(morsel starts, morsel counts, values)` triples, pymizu's publish `(element ranges, values)` pairs; element ranges and morsel pairs convert through the region's morsel size, so the lost-set scan and the splice work over either.

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
On Linux a free-list miss with lent bytes past `MIZU_ZC_LENT_BUDGET` sets the handle's churn flag and staging degrades to the copy tiers — below the budget a fresh create (pre-faulted, SIGBUS-proof) beats the copy floor, past it the copy tiers' deterministic reuse caps tmpfs growth.
Sweeps and force-reclaims clear the flag once reclaims bring lent bytes back under the budget; a pool submitter sweeps at submit time while churned (the busy-path sweep is worker-loop-only, and its pops stop missing once the copy tiers take over), so the fallback lifts on the first reclaim instead of never.

If the consumer dies, its counts leak.
The death verdict on the producer then force-reclaims its lent regions.
Ledger keys carry the consumer's kind (worker or submitter slot) in the high half, so a pool verdict reclaims exactly one namespace — a worker handle lends result regions to submitters and task-arg regions to workers under colliding slot numbers.
REFHELD-flagged regions (their identifiers escaped by reference) leak and unlink instead.
On a ledger overflow, the entry closes the mapping but keeps the name, so a REF in flight can still resolve.
The unlink lands at handle teardown.

## Zero-copy views

A layout-eligible object past the floor of the binding stages as SHM_VEC.
The stage is one layout write into a spill region.
The consumer wraps the mapped pages as a view instead of copying.
The cross-process refcount lives in bytes [24-31] of the region header.
The offset macros are wire format (`MIZU_ZC_REFCOUNT_OFF` / `MIZU_ZC_FLAGS_OFF` in `mizu.h`); the `mizu_zc_rc` / `mizu_zc_flags_` accessors are core-owned binding surface (`mizu_ext.h`, dual-form).
The producer stores 1 at stage.
`mizu_shm_open_view` adds 1 at the wrap of the consumer: open implies counted, so add-before-consumer-done is structural.
The `MIZU_OPEN_VIEW_NOCOUNT` form splits the two for a binding whose wrap can fail between map and count (an R ALTREP wrap can longjmp): open first, then `mizu_zc_ref` at wrap, still before the consumer-done signal.
The release of the view subtracts 1.
The consumer maps page 0 read-write (the refcount word) and the rest read-only.

## Crash atomicity

Each cross-process claim is copy-then-CAS, so a death mid-operation cannot wedge a queue.
A worker announces its claim (`in_flight_rs`) before it CASes.
The announce carries the entry's own incarnation stamp (the result-slot sequence at submit), so a reaper, serialized by the liveness lock, fails the slot only while the announced incarnation is still current.
An entry a dead claimant left queued is a zombie: bound to a terminated incarnation, it drops at execute without running, consuming, or stamping `worker_slot`.
On the controller, where the kept-fd probe and the death-callback thread share one open file description, a per-slot reap-owner word admits exactly one reaper — the flock re-acquire on one OFD is not a mutual exclusion.
On Linux a region create fails fast when a statvfs probe shows the region cannot fit in tmpfs; the reservation itself (a one-fault-per-extent touch + `MADV_COLLAPSE` huge-folio create on kernels >= 6.1, `MADV_POPULATE_WRITE`, `posix_fallocate` on kernels < 5.14) remains the enforcement point, so no later write fault can SIGBUS.

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

- `include/mizu.h`: the stable consumer API.
  Handles are opaque.
  Nothing public passes or returns a struct by value.
  Extensible structs carry a size field.
  Each public operation is a real exported function (`MIZU_API`).
  The shared library carries a soname that tracks `MIZU_VERSION_MAJOR`; the stable promise starts at 1.0.
- `include/mizu_ext.h`: the binding-author API, installed but version-pinned per minor release.
  It may change on any minor bump, without deprecation, ever — including after 1.0.
  Consumers are language bindings; they rebuild (or re-vendor) per minor release.
  Real exported functions are the rule.
  A `static inline` is permitted for the small pure helpers a binding wants inlined — byte math, wire-struct accessors, unit conversions.
  Every consumer-callable inline is dual-form: paired with a same-named exported symbol (`src/ext.c`), so FFI consumers see everything; header-internal building blocks (the `mizu_ext_*_impl` bodies and small predicates) stay inline-only.
  Cold protocol code is exported-only: the interop cursor, and the task decode shim of the byte-shape helper registry (whose form discipline the Interchange codec section states).
  `struct mizu_shm_s` is exposed here, layout-pinned per minor release: bindings dereference it on hot paths where an accessor call would not amortize.
- `src/internal.h`: private, never installed.
  The handle struct definition, spin machinery, and spill/ledger/open-cache internals live here.

Builds use `-fvisibility=hidden`.
Two contracts are versioned, both documented in the `mizu.h` preamble: the wire-format `MIZU_ABI_VERSION` and the soname.
The wire-format structs in `mizu.h` are the preamble-versioned contract, not soname-frozen.
