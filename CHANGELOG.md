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
- `MIZU_PYMIZU_CODEC_MAGIC` in `mizu_ext.h`; the keeperless gate recognizes it.
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

- `mizu_stage_fn` gains the binding ctx; `mizu_exec_fn` receives a read
  ctx in place of the bare binding ctx.
