# Changelog

All notable changes to libmizu are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versions carry an ABI contract: any wire-format change bumps
`MIZU_ABI_VERSION` in `include/mizu.h`, and the soname tracks
`MIZU_VERSION_MAJOR`.

## [Unreleased]

Pre-release; no tagged version yet. On the first tag, rename this section
to `## [0.1.0] - <date>` and start a fresh `Unreleased` section above it.

### Added

- Ordered factors (the new `MIZU_CAP_ORDERED` capability bit, 8): the
  factor attr shape whose `class` is the two-element strv
  `["ordered", "factor"]`, on the inline attr form, the layout attribute
  blob, and the Arrow frame writer — an Arrow dictionary column with
  `ARROW_FLAG_DICTIONARY_ORDERED` set no longer declines for a peer
  advertising the bit, its blob carrying the two-element class. New
  upstream surface: the `MIZU_CAP_ORDERED` and `MIZU_IX_CLASS_ORDERED`
  constants. Writers gate every emission on the peer's bit (decline,
  never strip); pre-cap readers fail loud on their exact-set guards. No
  ABI bump: caps are negotiated.

- Notify endpoints: an event-driven wake for a binding's async
  integration. An armed consumer owns a named, pollable endpoint (a
  bound filesystem datagram socket under the liveness dir on POSIX —
  `/dev/shm` on Linux, so the endpoint shares the regions' visibility
  scope exactly — a path past the `sun_path` budget fails the arm with a
  recorded error; an auto-reset named event on Windows); the publisher
  signals it at the same already-synchronized
  checkpoint where it tests the parked announce. The flag is
  edge-triggered and rides the word the publish already loads and tests
  (the channel's `parked` word, now a `MIZU_WAIT_*` bitmask; the pool
  result slot's `waiter_slot` as `MIZU_WS_NOTIFY`, coalesced per
  submitter through the new `mizu_sub_slot.notify` word), so the unarmed
  publish path is instruction-identical and an armed burst costs one
  signal. The watch is a persistent pure-C callback registration on the
  death-listener machinery (Linux's epoll thread, macOS dispatch sources,
  Windows thread-pool waits); a channel watch additionally chains the
  peer's death watch onto the same callback, and the close paths signal
  the endpoint alongside the parker unpark, so an armed consumer's drain
  also surfaces CLOSED and PEER_GONE. New ext-tier verbs:
  `mizu_channel_notify_arm` / `mizu_channel_notify_watch` /
  `mizu_channel_notify_unwatch`, `mizu_pool_notify_arm` /
  `mizu_pool_notify_watch` / `mizu_pool_notify_unwatch`, and
  `mizu_pool_task_notify` (the per-result-slot announce/withdraw a
  binding needs to arm individual tasks). New dump counters
  `notify_sends` on `mizu_channel_info` and `mizu_pool_dump`. No ABI
  bump: the layouts are unchanged (bit semantics on existing words, a
  word carved out of `mizu_sub_slot` pad) and pre-release pins move in
  lockstep as usual.

- The labeled attribute shapes (the new `MIZU_CAP_LABELED` capability
  bit, 7): a named atomic vector (attrs exactly `names`, NA elements
  carried), a dim array with `dimnames` — both the unnamed-axes list
  form and the named-axes form (the `dimnames` list itself attributed
  with exactly `names`, the one site a names-attributed list is not a
  0x0d dict), with an optional `class = "table"` — and the extended
  frame dict: a class vector whose last element is `"data.frame"`
  (tibbles and user frame classes round-trip) plus extra attributes
  outside the reserved set, emitted byte-sorted after the canonical
  keys, an attributed extra's value crossing as an opaque
  `attr(value, dict)` subtree (any encodable attribute set, byte-sorted
  keys). New upstream surface: the `MIZU_IX_ATTR_DIMNAMES` /
  `MIZU_IX_ATTR_TSP` / `MIZU_IX_CLASS_TABLE` constants, the dual-form
  `mizu_ix_attr_reserved(key, len)` (the reserved-key set, one list for
  both bindings), and `mizu_ix_skip(cur, &start, &len)` — an exported
  cursor verb consuming one complete value and reporting its byte span,
  so a binding can stash an extra's subtree verbatim and re-emit it
  without a decode. Writers gate every emission site on the peer's bit
  (decline, never strip); pre-cap readers fail loud on their exact-set
  guards. No ABI bump: caps are negotiated.

- `mizu_channel_abandon` / `mizu_pool_abandon`: the forked-child teardown
  for an inherited handle. Each releases only the process-local half —
  the child's copies of the mapping and fds plus its heap — and writes no
  cross-process word (no close signal or shutdown broadcast, no unlink,
  no slot release, no parker or refcount change), so the creating
  process's handle and the wire state are untouched. A binding finalizer
  can now release an inherited handle instead of leaking it (valgrind's
  default `definite,possible` leak kinds count such leaks as errors).
  Calling abandon in the owning process remains a contract violation, as
  destroy on an inherited handle was and remains.

- The Arrow frame writer's inline small-table form: `mizu_arrow_write_ix`
  (one complete `'I'` stream of the whole frame — header, attr mark, the
  list of the columns' value forms, then the root dict `{names, class =
  "data.frame", row.names = c(NA, -n)}`) and `mizu_arrow_write_ix_col`
  (one column's value form), both dst-NULL-sizing and built on the
  per-column writers and `mizu_arrow_str_at`, the string walk's malformed
  record put as NA like a null. A below-gate frame now crosses with one C
  write straight off the Arrow buffers where it previously converted back
  to a language value first; the stream is byte-identical to the bindings'
  historical inline emitters (the root dict is always the automatic form —
  the root-blob override is the region writer's — and the MIZL-only leaves
  never appear). No ABI bump: a same-language reader already accepts an
  `'I'` stream on the value tiers (the first-byte dispatch precedes the
  private codecs).

- The Arrow frame writer's per-column ext writers `mizu_arrow_col_body`
  (a column's by-value body bytes over every batch — the same loops the
  MIZL leaves use) and `mizu_arrow_col_blob` (its attribute dict as a
  bare `0x0d` dict value, the §3.5 blob without its stream header), both
  dst-NULL-sizing, so a binding's own `'I'` emitters share the core's
  byte-level code instead of copying it. The validity and per-element
  string accessors move to `mizu_ext.h` as the dual-form
  `mizu_arrow_valid` / `mizu_arrow_str_at`. `mizu_arrow_col` shrinks to
  the particulars the emitters still key on (`kind`, `row`, `str_form`,
  `u64_verbatim`); the dictionary levels are no longer copied out at
  open (validation walks batch 0's borrowed dictionary, which the level
  match and the factor blob now read directly). The frame-size memo
  re-runs after any override (`mizu_arrow_remote` / `_root_blob` /
  `_tz`), so overrides no longer need to precede the first size call.

- The Arrow frame writer's verbatim uint64 form: a `'L'` (uint64) column
  for a peer advertising the new `MIZU_CAP_MIZL_U64V` capability bit
  (bit 6) stages as an INT64 leaf — the identity lanes, the validity
  machinery unchanged — whose attrs blob is `{u64 = TRUE}` (new
  vocabulary macro `MIZU_IX_ATTR_U64`); a peer short of the bit gets the
  REAL conversion as before. `mizu_arrow_warn` gains `n_int64` (a
  genuine `INT64_MIN` on a masked int64 identity column — reads as NA in
  R), counted beside `n_intmin` on the masked walk only with the
  verbatim rows excluded (a twin `CVT_ROW_I64_V` keyed out by pointer
  compare); `mizu_arrow_stage` zeroes the struct, so existing
  initializers are unaffected. `mizu_arrow_col` gains the
  `u64_verbatim` out-field for a binding's own emitters. No ABI bump
  (capability bits are append-only); the fuzzers need no rows
  (`fuzz_arrow` opens caps-less, so its `'L'` rows stay on the
  conversion path, as its TSN rows do).
- The validity section's semantics are normative in DESIGN.md ("validity
  as data"): when a MIZH/MIZL header's validity pair is not `{0, 0}` the
  section is the authoritative null record and the in-band sentinels are
  the null-slot fill, with the fill invariant (a writer emitting a section
  writes the wire type's sentinel at every clear bit), the reader rules
  (a carried bitmap applies verbatim at null-concept boundaries; an absent
  section derives lazily from the sentinels — one rule for every atomic
  wire type, REAL's test the payload not `isnan`) and the writer rules
  (emit when the source carries a null record; `{0, -1}` only on data
  known null-free). The uint64 verbatim form is specified beside it: a
  `'L'` frame column stages as an INT64 leaf with a u64 marker attr for a
  peer advertising `MIZU_CAP_MIZL_U64V` (bit 6), both bindings advertise,
  and the degradation for pre-cap readers is channel-scoped. No byte
  change from the validity declaration: the section needs no capability
  bit and `MIZU_ABI_VERSION` is untouched.

- The Arrow frame writer (`src/arrow.c`): an Arrow C Data Interface →
  MIZL writer in the core, promoted from pymizu's `shmframe.c` so both
  bindings stage frames through one implementation. The Arrow structs are
  defined in `mizu_ext.h` under the spec's two include guards
  (`ARROW_C_DATA_INTERFACE`, `ARROW_C_STREAM_INTERFACE`). The interface:
  `mizu_arrow_open` (classify + batch vetting + dictionary validation,
  the decline in the thread-local slot naming the column, its format and
  the remedy), the `mizu_arrow_remote` / `mizu_arrow_root_blob` /
  `mizu_arrow_tz` overrides, `mizu_arrow_size`, `mizu_arrow_stage` (the
  one-call stage over the handle seam — the capability conjunction and
  the churn check now have one home), `mizu_arrow_col_info` (the
  per-column plans for a binding's own emitters) and `mizu_arrow_close`,
  with the `mizu_arrow_warn` conversion counts out-param. The fixed-width
  conversion kernels export as `mizu_cvt_for_arrow` / `mizu_cvt_convert`
  over the `mizu_cvt_row` table (`MIZU_CVT_*`). The type map: bool →
  LGL, int8/int16/uint16 → INT, uint8 → RAW (nulls decline), int32 →
  INT, uint32 → REAL, int64 → INT64, uint64 → REAL (past 2^53 → NA,
  counted), float16/32/64 → REAL, utf8/large_utf8/string_view → MIZS,
  dictionary<strings> → factor (ordered declines), date32 / timestamp /
  duration → REAL + the Date / POSIXct / difftime blobs. The producer's
  validity bitmap is authoritative (copied verbatim for a single offset-0
  batch, rebuilt honouring `batch->offset` otherwise) and the data
  carries the R sentinels at null slots — the write is always the foreign
  form (bulk CE_UTF8 marks + the validity tail), so the bytes are
  peer-agnostic. No wire change: `MIZU_ABI_VERSION` is untouched.

- The Arrow frame writer's nanosecond temporal form: a `timestamp[ns]` /
  `duration[ns]` column stages as a verbatim INT64 leaf (the counts
  unrescaled, `NaT` the INT64 sentinel) whose attrs blob adds
  `tscale = "ns"` beside the POSIXct/`tzone` or difftime/`units` pairs,
  gated per column on the new `MIZU_CAP_MIZL_TSN` capability bit (bit 5)
  — a peer short of the bit gets today's REAL-seconds write with the
  `1e-9` rescale, and a reader short of it fails informatively on the
  form, never a silent mis-read. New attr vocabulary macro
  `MIZU_IX_ATTR_TSCALE`. The verdict resolves once at classify
  (`peer_caps` is already in hand), so the size and write passes cannot
  diverge. No ABI bump: capability bits are append-only and absent means
  unsupported.

- The byte-shape helper registry (DESIGN.md's Interchange codec section):
  the 'I' machinery that operates purely on byte spans and cursor items
  moves into the core so no byte-level rule keeps a second implementation
  free to drift across bindings. New dual-form helpers
  `mizu_ix_utf8_valid` (strict RFC 3629 validation — the cursor's own
  string checks now single-source through it), `mizu_ix_tag_of` (the
  wire-type → vector-tag table; `mizu_ix_put_vec`'s internal switch
  single-sources through it), and `mizu_ix_write_err` (the bounded err
  framer — type capped at its 128-byte share, message at half the inline
  budget, detail at the remainder, each cut at a UTF-8 boundary, so the
  stream fits the slot by construction and cannot fail), with the budget
  constants `MIZU_IX_ERR_OVERHEAD` / `MIZU_IX_ERR_TYPE_SHARE` in the
  header as the wire authority. New exported-only task-stream decode
  shim `mizu_ixt_open` / `mizu_ixt_want_code` / `mizu_ixt_want_list` /
  `mizu_ixt_want_dict` — the per-field shape checks every task decode
  shares, recording the normative texts in the thread-local slot so they
  are byte-identical across bindings by construction. New attr
  vocabulary macros `MIZU_IX_ATTR_*` / `MIZU_IX_CLASS_*` /
  `MIZU_IX_UNIT_*` (the shape whitelist's 7 keys, 6 class strings and 5
  difftime units; append-only). No wire change: the golden corpus passes
  unmodified and `MIZU_ABI_VERSION` is untouched.

- The MIZL remote leaf (directory tag 33) is normative in DESIGN.md (the
  follow-ups plan's F2 — a per-column REF): the entry's `data_offset` /
  `data_size` hold the view layer's identifier span (the 0x13 ref leaf's
  byte form, 1–255 bytes), `length` and `attrs_size` describe the
  referenced column as resolved, the S4 bit rejects, and the validity
  pair is a `{0,0}` / `{0,-1}` claim alone — a remote column contributes
  no bitmap and no count to the header validity (the `vcount` bound over
  local atomic leaf lengths forces the exclusion), and a `{0,-1}` claim
  is validatable only against a referenced form that records known-NA-
  free itself. Resolve-time validation compares the claims against the
  resolved leaf; a dangling identifier, a parse failure or any mismatch
  declines as a corrupt region. Emission gates on the new
  `MIZU_CAP_MIZL_REF` capability bit (bit 4), per frame. The layout
  checks admit the tag: `mizu_ext_mizl_tag_ok` lists it,
  `mizu_ext_mizl_ent` exempts it from the attrs-tail and body checks and
  bounds the span, and `mizu_ext_mizl_elem` rejects a nonzero validity
  offset on it. New `fuzz_mizl` harness over the directory parser. No
  ABI bump.

- The ref leaf (`'I'` tag 0x13) is normative in DESIGN.md (the
  follow-ups plan's F1 — task arguments by reference): u8 length (1–255)
  plus the view layer's identifier bytes, an ordinary value in the
  cursor's grammar (the cursor checks the length and bounds only), taken
  by the task-stream exec decode, the map descriptor reader and the
  collect-side result reader, and declined informatively everywhere
  else. The task prose gains the retain protocol: the submit-side pin to
  the worker's counted-add handoff (no keeperless claim on a
  ref-carrying frame), the worker's emitted-set release discipline after
  the outcome write, the consumer-death rule for a kill mid-task, and
  the single-checkout SHM_VEC constraint with its size-pass-first
  resolution. Emission gates on the new `MIZU_CAP_TASKREF` capability
  bit (bit 3). New dual-form emit helper `mizu_ix_put_ref`; the cursor
  gains the 0x13 case; the corpus pins task rows with ref arguments, the
  builder decline at a channel value site, and the empty / truncated
  cursor rows.

- The task kind registry's kind 2 (runner) row and the cross-language
  map's wire forms are normative in DESIGN.md (Phase 5): the runner's
  fields — region name, the ordinal and morsel generation packed in one
  i64 (ordinal the high 32 bits), the seed as nil or the
  `(seed, offset)` i64 pair — the `'I'` map descriptor
  (`list[task, x | nil]`, the f spec nested as a kind 0/1 task tag), the
  one `MIZU_MORSEL_MAGIC` with the descriptor's codec identity riding
  its own first byte, the per-binding runner result shapes with the
  foreign-collect normalization rule, and the per-element map error's
  index on the err tag. No code change (the cursor has parsed kind 2
  since Phase 0); the implementing phases are the bindings'.

- The task tag (0x12) format is normative in DESIGN.md (Phase 4): the
  fixed-offset header (target, kind, reserved-zero flags, submitter
  identity) with the SHM_RAW resolution rule, the misroute guard
  ("task language mismatch"), the submitter identity as the ERR-format
  and result-policy key with the foreign writer policy for results, the
  no-keeperless rule for task streams, the source-kind
  trailing-expression convention, and the neutral error stream for a
  foreign private frame at a worker. The corpus gains `task(...)` rows
  and the wrong-shape / not-a-value / truncation read-err rows; no code
  change (the cursor has parsed the tag since Phase 0).

- The interchange codec's byte-level half (the execution plan's item
  1.6a): `src/interop.c`, the validating pull cursor for the `'I'`
  stream (`mizu_ix_open` / `mizu_ix_next` / `mizu_ix_end`, ext tier) —
  bounds, the depth cap, UTF-8 validity, the container arity accounting
  (a task is one element of kind-determined arity), the reserved-flag
  rejections, and the informative unknown-tag / unknown-version /
  unknown-kind declines ("the peer uses a newer format") in the
  thread-local error record — plus the `mizu_ix_put_*` emit helpers
  (dual form) a binding's two-pass walk sizes and writes through. The
  golden conformance corpus (`tests/interop/cases.txt` → `corpus.txt`
  via the spec-derived stdlib reference `tools/interop_corpus.py`;
  deterministic, CI regenerates and asserts a clean diff) certifies the
  byte grammar once here: `tests/unit/test_interop.c` drives it through
  the cursor and re-emits it byte-for-byte, and the cursor joins the
  fuzz tier (`tests/fuzz/fuzz_interop.c`).
- The err tag (0x11) is normative (the execution plan's Phase 2):
  DESIGN.md's interchange codec section specifies its internals — the
  top-level-only error value (u16 flags, the optional u64 element
  index, the three bare strings type / message / detail) and the
  bounded writer: each string truncates at a UTF-8 boundary past its
  share of the slot's inline budget, so the frame fits by construction,
  stamps INLINE with the keeperless claim, and cannot fail. The golden
  corpus pins the first err round-trip fixtures.
- The cross-language wire contract (the execution plan's Phase 0).
  Pre-release: `MIZU_ABI_VERSION` stays 1 (peers are same-build; an old
  reader fails safe) — this entry, like the INT64 entry's, records the
  exception to the header's bump-on-any-wire-change rule.
  - The registries: `'I'` (0x49, `MIZU_INTEROP_MAGIC`) joins the codec
    registry (pickle 0x80 listed with it, and readers dispatch on the
    first byte of every serialize tier); the `MIZU_LANG_*` language
    registry (0 none / 1 bytes / 2 R / 3 Python); the `MIZU_CAP_*`
    reader-capability bits (MIZS, ATTRS, MIZL — a bit names a byte
    layout, never reassigned); the `MIZU_IDENT()` identity word
    (language in bits 0-7, capabilities in bits 32-63, reserved bits
    8-31 ignored). Reserved, with formats to follow in their own phases:
    `'I'` tags 0x11 (err) and 0x12 (task), and MIZL directory-entry
    sexptype tag 33 (the remote leaf). The `MIZU_TYPE_*` numbers are
    frozen from the first release (SEXPTYPE origin is history).
  - Identity words on the wire: each channel side publishes its
    binding's word in its entity block (`MIZU_ENTITY_IDENT`; the host at
    create, the peer at attach before `ready_set`); the pool header's
    `pad[8]` becomes `worker_ident`, fixed by the first worker join's
    CAS and exact-match ever after (a differing word fails the join; the
    word is never reset). `mizu_binding` gains the `ident` field; a zero
    language byte is rejected at create, attach and join — the bytes
    binding declares `MIZU_IDENT(MIZU_LANG_BYTES, 0)`. New getters
    `mizu_channel_peer_ident()` / `mizu_pool_worker_ident()` read the
    words through the mapping.
  - The optional validity-bitmap section: header words [40-47]/[48-55]
    of every MIZH and MIZL header — {0, 0} absent, {0, -1}
    known-NA-free, else a 64-byte-aligned bitmap (MIZH) or leaf-entry
    table (MIZL). The layout headers are now documented in mizu.h: the
    format flags word at [32-35] (bit 0 S4; an unknown set bit rejects),
    the MIZS string block, the 32-byte MIZL directory entry (64-aligned
    `data_offset`; S4 bit 30; tag 32 legal; tag 33 reserved), and the
    `MIZU_CE_*` encoding constants beside `MIZU_STR1_NA`.
  - New ext-tier symbols (dual form): `mizu_mizh_validity_set`,
    `mizu_mizs_geometry`, `mizu_mizs_check`, `mizu_mizl_check`,
    `mizu_mizl_elem` — every check rejects a misaligned directory
    `data_offset`, an unknown flags-word bit and an unlisted sexptype —
    and the `mizu_na_build` / `mizu_na_apply` sentinel<->bitmap
    primitives (one NA-test implementation behind both bindings'
    validity paths).
- `MIZU_TYPE_INT64` wire tag (32) for int64 payloads; INT64_MIN is the
  missing sentinel. Pre-release: `MIZU_ABI_VERSION` stays 1 (peers are
  same-build; an old reader fails safe — elt size 0 is a corrupt-slot
  error, never a misread).
- Three-tier API: stable `mizu.h`, the binding-author `mizu_ext.h`
  (installed, version-pinned per minor release), and a private
  `src/internal.h`. The callback seam, stager/read/publish services,
  bytes binding, and promoted internals moved out of `mizu.h`; the
  dual-form accessors (`mizu_parker_snapshot`, `mizu_zc_rc`,
  `mizu_zc_flags_`) ship as header inlines plus exported symbols.
- SPSC channel transport over POSIX shm and Win32 file mappings.
- Work-stealing task pool transport: vectored collects, introspection,
  map support, armed death watches, `mizu_pool_task_release`,
  `mizu_pool_submit_batch_fn`.
- Built-in bytes binding; callback seam (`stage`, `read`, `exec`,
  `check`, `park`, `sweep`, `drop`) for external language bindings.
- Lock-free wire formats with crash atomicity; parker protocol with
  platform waiters (futex, `__ulock`, WaitOnAddress).
- Zero-copy payload tiers with spill/ledger machinery and retain table.
- Three-file amalgamation distribution (`mizu.c` + `mizu.h` + `mizu_ext.h`).
- pkg-config support via `make install`.
- `MIZU_PYMIZU_CODEC_MAGIC` in `mizu_ext.h`.
- `MIZU_AUX_F_KEEPERLESS`, bit 0 of the INLINE aux word: a stager-authored
  claim that staging a frame committed no retain-table entry. The pool
  collect keeperless gate reads the header claim instead of probing
  payload codec magics; the bytes binding and `mizu_result_publish_err`'s
  INLINE branch stamp it. Wire-compatible without an ABI bump: old cores
  never read INLINE aux, and a clear bit is always correct (at worst a
  spurious keeper-sweep wake).
- `mizu_shm_open_view_flags` with `MIZU_OPEN_VIEW_NOCOUNT`.
- `MIZU_READ_CONSUME` read flag: a failed read with the flag set consumes
  the slot while the verb returns `MIZU_ERR`.
- `MIZU_NA_INT32` / `MIZU_NA_INT64` / `MIZU_NA_REAL_BITS` sentinels.
- Wire helpers in `mizu_ext.h`: `mizu_timeout_ms`, `mizu_store_na_real`,
  `mizu_aux_rawspill_pool`, `mizu_aux_shm_vec` with the `mizu_aux_type` /
  `mizu_aux_hi` decode pair, `mizu_mizh_write`, `mizu_mizh_check`.
- `mizu_stage_raw` in `mizu_ext.h` (dual-form; slow path `mizu_stage_raw_spill`
  in `stage_raw.c`): the core-owned raw-tier staging reservation — the
  RAWVEC / arena-RAWSPILL / region-RAWSPILL / flat-SHM_VEC cascade both
  first-party bindings staged by hand. One deliberate behavior delta
  against mizu's former cascade: a channel SHM_VEC region-creation failure
  retries the arena before the reservation declines to the serialized
  tiers.
- The map morsel protocol in `mizu_ext.h` (`morsel.c`, `mizu_morsel_*`): the
  128-byte map header (one unified layout; bindings keep their magic tags),
  the generation-fenced CLAIM word protocol, AIMD batch sizing
  (`mizu_morsel_sizer`), reset/re-arm, the abandon trim, the cancel word, and
  the lost-set scan — extracted from mizu's and pymizu's map modules.

### Changed

- **Wire format (128-byte cache-line discipline):** no two words
  written by different processes now share a 128-byte span in any region
  layout (Apple Silicon's line size; Intel's adjacent-line prefetcher
  couples 128-byte sibling pairs too). The channel's ring indices re-pair
  by writer — [256,384) is host-written only (`MIZU_OFF_HP_TAIL`,
  `MIZU_OFF_PH_HEAD`), [384,512) peer-written only (`MIZU_OFF_PH_TAIL`,
  `MIZU_OFF_HP_HEAD`) — at zero cost. The pool layouts widen: the
  injection ring meta to 256 B (`MIZU_INJ_HEAD_OFF` 128), the tier words
  to 256 B (`MIZU_TIER_FULL_OFF` 128), `mizu_wk_slot` to 256 B (admin +
  owner-written words on the first span, the thief-CAS'd `deque_top` on
  the second), `mizu_sub_slot` to 128 B (one submitter per span), and the
  control block to 384 B (`MIZU_CTRL_PARKED_OFF` 128,
  `MIZU_CTRL_HELP_OFF` 256); every section base in the pool region rounds
  to 128 so the widened structs never straddle. The channel `arena_size`
  contract tightens to a multiple of 128 to keep the second arena's base
  aligned. Pre-release: `MIZU_ABI_VERSION` stays 1 (peers are
  same-build) — the standing exception to the bump-on-any-wire-change
  rule; both bindings re-vendor.

- uint64 conversion: values up to `INT64_MAX` now cross exactly as
  integer64 (an INT64 leaf / the `i64v` form) instead of double with
  past-2^53 lanes reading as `NA_real_`; only values past `INT64_MAX`
  keep the loss event, now the `INT64_MIN` sentinel, still counted in
  `mizu_arrow_warn.n_range`. The conversion-table row
  `MIZU_CVT_U64_REAL` is replaced by `MIZU_CVT_U64_INT64` (same table
  slot); the fallback's wire type is baseline (`MIZU_TYPE_INT64`), so no
  capability bit gates it and pre-existing readers home the more exact
  form natively. The verbatim uint64 form (`MIZU_CAP_MIZL_U64V`) is
  unchanged.

- **Wire format (pool):** the injection/deque
  entry header carries the result slot's incarnation sequence
  (`rs_seq`) — `mizu_entry_hdr` grows to 40 bytes with the payload at
  offset 40 — and the result states gain `MIZU_RS_LOST`
  (`mizu_pool_status.tasks_by_state` grows to 7 entries). Pre-release:
  `MIZU_ABI_VERSION` stays 1 (peers are same-build; an old reader fails
  safe) — the standing exception to the bump-on-any-wire-change rule;
  both bindings re-vendor.
- Ext tier: `mizu_result_publish_died` is renamed
  `mizu_result_publish_lost` and publishes `MIZU_RS_LOST` — the
  payload-vanished terminal no longer carries a worker-death verdict
  (the claimant is alive; it is publishing), so the collect claim
  performs no force-reclaim of the claimant's lent regions and builds
  no claimant record (`died_slot` -1, `died_pid` 0). `MIZU_RS_DIED` is
  once more the lock-verified reaper's terminal alone.
- Version macros 0.1.0 (the ext tier's pin for the rename).

- `mizu_arrow_open`'s duplicate-name check is O(ncols) — an
  open-addressed table over the name hashes, `strcmp` on a hash hit
  only — replacing the quadratic pair scan a wide frame paid per send.
- `mizu_morsel_layout` / `mizu_morsel_hdr_check` lose the magic
  parameter: the morsel module stamps and checks the one core-owned
  `MIZU_MORSEL_MAGIC` (0x4D495A4D — mizu's value, so only pymizu's map
  regions change magic; the descriptor's codec identity rides the
  descriptor stream's own first byte). Ext-tier signature change.
- `mizu_mizh_check` gains the `valid` out-param (the MIZH validity
  section, bounds-checked and handed back). Ext-tier signature change.
- `mizu_channel_recv_batch_fn` returns every message it consumed: a read
  failure after the first message now ends the batch early with `MIZU_OK`
  and the prefix instead of `MIZU_ERR` dropping it, and the failing slot
  stays at the head so the next receive reproduces the failure (and makes
  the consume decision — `MIZU_READ_CONSUME` is honoured on a single
  receive and a batch's first message only). No wire-format change.
- `mizu_pool_collect_all_fn` on a first non-OK outcome now claims the
  reported handle alone: the OK results ahead of it are no longer
  consumed and dropped — every other handle stays collectible, symmetric
  with the timeout case. No wire-format change.
- `mizu_pool_leave` now fails the worker's announced in-flight claim as
  DIED: an orderly leave after an exec_fn infrastructure failure no longer
  strands the claimed task PENDING — the collector gets the worker-death
  verdict without waiting on a reaper. No wire-format change.
- `mizu_stage_fn` gains the binding ctx; `mizu_exec_fn` receives a read
  ctx in place of the bare binding ctx.
- `mizu_pool_collect_all` / `mizu_pool_collect_all_fn` gain a
  `consumed_out` param: a read failure after an all-OK wait returns
  `MIZU_ERR` with the failing slot's index in `err_index_out`
  (unconsumed when retryable) and the number of leading results already
  handed to the sink / written to `values_out` in `consumed_out` — the
  consumed prefix the bindings previously discarded (R-12 / P-04 adopt
  it). `n` on a fully collected run, 0 when the run reports a non-OK
  outcome or times out. Ext-tier signature change; no wire-format
  change.

### Fixed

- An armed async pool submitter is now woken when the owner dies with
  no survivor to broadcast: `mizu_pool_notify_watch` chains the owner's
  death watch onto the notify callback (as the channel chains its
  peer's), where before an owner dying together with all its workers —
  a process-group kill, or workers already gone — left the armed drain
  asleep until its own timeout or teardown.
- `mizu_channel_destroy`'s drain rendezvous is now woken by the drain
  itself: the consumer's head publish runs the flush-side Dekker
  (fence, then an unpark when the peer's parked word announces the
  drain wait and the ring has caught up), where before only a peer
  send, close, or confirmed death unparked the wait — a peer that
  merely read late, sending and closing nothing, cost the whole 1000 ms
  bound on every destroy with an undrained message.
- A notify signal can no longer be lost to a sender-side open failure:
  both the channel's `chan_notify_signal` and the pool's
  `pool_notify_send` now open the cached tx before claiming the edge,
  where claiming first cleared the consumer's announce and then could
  return without sending — the announce stayed cleared until the next
  wake, which would never come (a missed wake on fd exhaustion). An
  open failure now leaves the announce in place and the next armed
  sighting retries. The pool's lazily allocated tx table is also
  serialized now: concurrent first-signals from worker publishes and
  the off-thread reaper raced the calloc and the per-submitter open
  (a leaked fd and a lost tx).
- Notify efficiency, three parts. (1) The pool's per-result publish to
  an armed submitter load-gates the coalescing-word RMW: a burst's
  first publisher wins the edge, every later one pays one shared
  acquire load instead of a locked RMW on the submitter's cache line.
  (2) Re-arming a continuously armed word skips the endpoint drain and
  the announce RMW — a datagram exists only once a publish clears the
  bit — so the steady-state re-arm is one load plus the protocol fence;
  the announce/re-check ordering is unchanged. (3) On Linux the
  endpoint socket is born `SOCK_NONBLOCK | SOCK_CLOEXEC`, dropping the
  four fcntls per endpoint.
- The Linux death listener no longer runs watch callbacks under its
  registry mutex: fired watches are snapshot under the lock and their
  callbacks invoked unlocked, so a slow callback (a binding's drain
  that takes its runtime's lock) no longer serializes every watch
  registration and callback in the process — a cross-lock deadlock
  against a registration holding that runtime lock is gone — and a
  callback may stop watches, its own included, without self-deadlock.
  Stop waits out an in-flight callback before returning, so the
  "nothing touches the watch after stop" guarantee teardown relies on
  is unchanged; a callback landing after a concurrent stop is the
  absorbed-stale-signal case the drains are built for. The per-batch
  registry reap sweeps are now gated on a stop having posted work.
- `mizu_channel_destroy` no longer unlinks in-flight payload regions
  under a live peer: a producer exiting right after publishing stranded
  the consumer's resolve with "payload region vanished" whenever the
  teardown's unlinks beat the consumer's read (the close rendezvous
  covered the control region's name only). Destroy now waits — bounded
  (1 s) and death-verdict gated — for the peer to drain the tx ring
  (head publication trails every resolve and copy-out) before the spill
  teardown runs: the close protocol's "in-flight drain is never raced"
  now covers payload regions. A dead peer falls through to the survivor
  cleanup as before; a stuck-but-live peer costs the bound, never a
  hang.
- `mizu_arrow_str_at` no longer declines a string_view span that
  references the array's trailing buffer slot: the C Data spec puts the
  int64 variadic-sizes array there, but nanoarrow's nested and standalone
  exports put a data buffer there instead, so every out-of-line string
  from such an export (any view longer than 12 bytes) read as malformed.
  A view naming the trailing slot now proves the no-sizes layout and
  crosses unbounded; any other span the sizes array bounds by the
  declared size, as before.
- `mizu_pool_lame_duck` no longer releases a non-worker handle: probing a
  controller or submitter returns nonzero without touching it. Previously
  any probe with an empty keeper table released the handle in place, and
  a controller released this way could never unlink — only its
  stop/destroy protocol removes the region and liveness files — orphaning
  the pool (and the creator's macOS registry log) irrecoverably.
- A worker dying between its claim announce and the claim CAS no longer
  lets the still-queued entry execute against the slot's next
  incarnation: the announce (and the sink) now carry the entry's own
  incarnation stamp, and `pool_execute` drops a claimed entry whose
  stamp no longer matches the slot (or whose status is already terminal)
  without executing, consuming, or stamping `worker_slot`. Previously
  the reaper failed the announced slot DIED, the submitter recommitted
  it, and the zombie's publish won the new incarnation's slot — the
  collector read the dead task's value for the new task.
- The lent-region ledger keys a worker handle's two consumer namespaces
  in one tagged space (`MIZU_LED_SUB` / `MIZU_LED_WK` in the key's high
  half): a worker-death force-reclaim of slot k no longer reclaims
  result regions lent to *submitter* k (still held by live views),
  which the next spill stage would have overwritten.
- `mizu_result_publish_lost` (née `_died`): a transient/torn payload
  open on a live thief no longer produces a `DIED` claim whose
  force-reclaim treated the live claimant's lent regions as leaked and
  whose error named a live worker as dead.
- The controller's kept-fd death probe and its off-thread death-callback
  reap are serialized by a per-slot reap-owner word: two reapers can no
  longer enter on the same open file description (flock re-acquire on
  one OFD always "succeeds", and the first finisher's unlock dropped
  the grant under the other — a respawn could then join mid-reap and
  have its fresh PENDING failed DIED by the straggler). The protocol is
  pure userspace CAS, so Windows same-handle `LockFileEx` semantics are
  irrelevant.
- The ring-claim backpressure wake now fences `memory_order_seq_cst`
  before the `full_waiters` load, like the other two pusher sites — a
  relaxed load after the head CAS had no Dekker guarantee on ARMv8 (a
  missed bit cost the submitter a bounded 2 s sleep).

- The string_view element accessor's buffer bound
  (`mizu_arrow_str_at`'s view form, the ext tier's dual-form impl): a
  view record's buffer index was admitted up to `n_buffers - 3`, which
  indexes the trailing variadic sizes array the re-viewing producers
  append as if it were a data buffer, and a span was never checked
  against the declared size. The variadic data buffers are slots
  `[2, n_buffers - 1)`: an index into the sizes slot is now malformed,
  and where the sizes array is present (non-NULL) a span past `sizes[bi]`
  declines — pymizu's `pv_col_match_strview` bound, promoted to the one
  shared definition. A frame carrying such a record declines at the
  open's size walk ("malformed offset or view record"), ahead of any
  emit. No wire change.
- `arrow_classify` declines a timezone name past 63 bytes with an
  informative error instead of silently truncating it into the POSIXct
  blob (no silent downgrades; the longest zoneinfo key is 32 bytes, so no
  legitimate payload meets the bound).
- `mizu_na_build`'s REAL and CPLX element tests now discriminate the
  `NA_real_` payload the way R's own `ISNA` does — any NaN whose low
  word is 1954 (`0x7A2`) — instead of matching only the quiet-bit-set
  `MIZU_NA_REAL_BITS` exactly, so a writer's quiet-bit-clear `NA_real_`
  (R's verbatim form, `0x7FF00000000007A2`) reads as NA on the
  validity-bitmap paths. No ABI or wire change (the helpers are
  unreleased, Phase 0).
- A host-side death probe before the peer has attached no longer reads
  a verdict from the peer's untaken liveness lock: `chan_probe_dead`
  gates on the ready word (host side only), so the full-ring
  disambiguation returns `MIZU_FULL` and `mizu_channel_alive` reports no
  verdict instead of unrecoverably unlinking the channel pre-rendezvous.
- A receive with the death-listener flag set but the lock probe reading
  HELD (a forked child or squatter outliving the peer holds the lock's
  open file description) no longer spins unboundedly at 100% CPU with
  the deadline and the interrupt hook unpolled: once the probe has
  returned HELD the one-shot flag stops vetoing the park, and the
  bounded park re-probes on every wake until the last holder exits.
- A fork-without-exec child (an mclapply worker, a multiprocessing
  child) no longer keeps a dead holder's liveness lock alive past its
  death: every lock fd the library hands out is tracked, and a
  `pthread_atfork` child handler closes the child's copies (closing one
  fd of a duplicated pair never releases the parent's flock).
  `mizu_live_close` is registry-gated, so a stale handle fd in the child
  can never close a descriptor since reused. POSIX only — Win32 handles
  were never inheritable.
- A consumer can once more drain to `MIZU_CLOSED` when the producer
  destroyed (or died) with a spilled payload in flight: the vanished
  region fails its one read, and a `gone` failure with the producer
  closed or verdict-dead is consumed exactly as `MIZU_READ_CONSUME`
  instead of wedging the ring behind the slot forever.
- An orderly submitter detach frees its registry slot: with nothing
  PENDING, handle teardown cancels nothing — it frees the
  published-but-uncollected results (releasing the producing workers'
  keepers, exactly as the reaper would) and marks the slot FREE ahead of
  the lock close. With PENDING work the slot stays LIVE and the next
  attach's claim scan probes it (identity-checked lock acquire, then the
  reaper's sweep) instead of skipping it — a live process is never
  denied attach by a dead registration.
- `mizu_pool_leave` now fails every result slot still stamped with the
  worker's slot, not just the announced in-flight claim: a nested claim
  overwrites the announce, and an orderly leave runs no death reaper to
  fail the overwritten claims — their collectors parked forever.
- A worker join failing after its slot CAS (parkers attach, scratch
  allocation, owner watch) hands the slot back to FREE instead of
  leaving it CLAIMING until the stop sweep.
- The Linux orphan prune scan now requires the exact region grammar
  (`mizu_<pid>_<counter>`, both hex): the liveness lock files sharing
  `/dev/shm` (`....live.host`, `....wk.0`, `....owner`, ...) are no
  longer enumerated as regions and unlinked from under live workers.
- `mizu_stage_raw_spill` rejects a non-atomic `wire_type` (no element
  size) at entry instead of dividing by zero in the flat SHM_VEC
  reserve — binding misuse declines to the serialized tiers.
- `mizu_live_fd_bounce` covers fds 1/2, not just 0: launched with
  stdout/stderr closed, a lock fd landing there would have been released
  by a later dup2/freopen onto it (a false DEAD).
- The lent-region ledger's quota sweep rotates its start across calls
  (`sweep_next`): four or more long-held regions at the ledger front no
  longer starve every entry behind them on the busy-path sweeps.
- Wire-carried region names are gated on the exact grammar (the prefix
  plus `<hex>_<hex>`) at `mizu_read_region` and
  `mizu_shm_open_view_flags` before any concatenation into `/dev/shm%s`:
  a `/../` name from a corrupt or hostile peer no longer opens an
  arbitrary same-user file.
- The Arrow writer's null-free string span (`arrow_size_str`'s utf8 /
  large_utf8 fast path) now walks every intermediate offset — only the
  endpoint pair was checked, so a non-monotonic interior (or an i64
  element span past the int32 element bound) sized one way while the
  writer's per-element rebase gathered another: a corrupt MIZS leaf on
  a single batch, a heap overflow once a later batch's null flipped
  the column to the gather.
- Every batch's dictionary now gets the buffer-shape vet at open, not
  just batch 0's: a later batch's malformed dictionary (NULL buffers,
  a short count) was read unvetted by the cross-batch level match.
- `convert_masked`'s full-word branch counts the genuine sentinels off
  the written lanes by per-lane `memcpy` instead of a typed dst scan:
  the inline 'I' form's column bodies are unaligned (a vec header
  precedes them), so the scan was undefined on exactly the tier that
  runs it.
- Short Arrow format strings no longer read past their NUL:
  `arrow_classify` compares the unit byte before indexing `f[3]`
  ("ts", "tD"), and `mizu_cvt_for_arrow` guards `f[0]` before `f[1]`
  ("").
- An Arrow batch carrying a struct offset or null struct rows (a
  non-NULL validity bitmap with `null_count != 0`) declines at open:
  the writer stages the children's slot values as rows and never reads
  the struct's own validity or offset.
- `mizu_arrow_remote` declines the override when the referenced leaf's
  length differs from the frame's row count — the tag-33 entry it
  would have written fails the reader's resolve as corrupt, instead
  of the column staying local (the torn-tree precedent).
- A dictionary column whose factor blob would exceed the int32
  `attrs_size` range declines at open (each level costs framing bytes
  beyond the already-bounded packed bytes, so a level count past
  ~2^29 wrapped the cast at write).
- `mizu_pool_create` now caps `result_slots` at 2^24 *after* the
  per-submitter partition rounding, rounding down when the carry
  crosses: a value at the documented cap whose submitter count did not
  divide it rounded *up* past the task handle's 24-bit `rs_index`,
  wrapping high indices into the sequence bits.
  `mizu_pool_hdr_validate` gains the matching bound.
- The Linux death listener no longer goes deaf when its epoll thread
  dies: the thread's abnormal exit fires every active watch
  conservatively (a wake trigger only — the liveness lock stays the
  verdict), unlists the set so `mizu_death_watch_stop` frees directly,
  and resets the shared state so the next registration starts a fresh
  listener. Previously `mizu_dl_running` stayed set, so no watch ever
  fired or restarted again.
