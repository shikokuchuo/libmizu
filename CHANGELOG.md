# Changelog

All notable changes to libmizu are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versions carry an ABI contract: any wire-format change bumps
`MIZU_ABI_VERSION` in `include/mizu.h`, and the soname tracks
`MIZU_VERSION_MAJOR`.

## [Unreleased]

Pre-release; no tagged version yet. On the first tag, rename this section
to `## [0.0.1] - <date>` and start a fresh `Unreleased` section above it.

### Added

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

### Fixed

- `mizu_na_build`'s REAL and CPLX element tests now discriminate the
  `NA_real_` payload the way R's own `ISNA` does — any NaN whose low
  word is 1954 (`0x7A2`) — instead of matching only the quiet-bit-set
  `MIZU_NA_REAL_BITS` exactly, so a writer's quiet-bit-clear `NA_real_`
  (R's verbatim form, `0x7FF00000000007A2`) reads as NA on the
  validity-bitmap paths. No ABI or wire change (the helpers are
  unreleased, Phase 0).

### Added

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
