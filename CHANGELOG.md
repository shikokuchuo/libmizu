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
