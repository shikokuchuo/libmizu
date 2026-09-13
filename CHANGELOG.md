# Changelog

All notable changes to librei are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versions carry an ABI contract: any wire-format change bumps
`REI_ABI_VERSION` in `include/rei.h`, and the soname tracks
`REI_VERSION_MAJOR`.

## [Unreleased]

Pre-release; no tagged version yet. On the first tag, rename this section
to `## [0.0.1] - <date>` and start a fresh `Unreleased` section above it.

### Added

- `REI_TYPE_INT64` wire tag (32) for int64 payloads; INT64_MIN is the
  missing sentinel. Pre-release: `REI_ABI_VERSION` stays 1 (peers are
  same-build; an old reader fails safe — elt size 0 is a corrupt-slot
  error, never a misread).
- Three-tier API: stable `rei.h`, the binding-author `rei_ext.h`
  (installed, version-pinned per minor release), and a private
  `src/internal.h`. The callback seam, stager/read/publish services,
  bytes binding, and promoted internals moved out of `rei.h`; the
  dual-form accessors (`rei_parker_snapshot`, `rei_zc_rc`,
  `rei_zc_flags_`) ship as header inlines plus exported symbols.
- SPSC channel transport over POSIX shm and Win32 file mappings.
- Work-stealing task pool transport: vectored collects, introspection,
  map support, armed death watches, `rei_pool_task_release`,
  `rei_pool_submit_batch_fn`.
- Built-in bytes binding; callback seam (`stage`, `read`, `exec`,
  `check`, `park`, `sweep`, `drop`) for external language bindings.
- Lock-free wire formats with crash atomicity; parker protocol with
  platform waiters (futex, `__ulock`, WaitOnAddress).
- Zero-copy payload tiers with spill/ledger machinery and retain table.
- Three-file amalgamation distribution (`rei.c` + `rei.h` + `rei_ext.h`).
- pkg-config support via `make install`.
- `REI_PYREI_CODEC_MAGIC` in `rei_ext.h`; the keeperless gate recognizes it.
- `rei_shm_open_view_flags` with `REI_OPEN_VIEW_NOCOUNT`.
- `REI_READ_CONSUME` read flag: a failed read with the flag set consumes
  the slot while the verb returns `REI_ERR`.
- `REI_NA_INT32` / `REI_NA_INT64` / `REI_NA_REAL_BITS` sentinels.
- Wire helpers in `rei_ext.h`: `rei_timeout_ms`, `rei_store_na_real`,
  `rei_aux_rawspill_pool`, `rei_aux_shm_vec` with the `rei_aux_type` /
  `rei_aux_hi` decode pair, `rei_reih_write`, `rei_reih_check`.
- `rei_stage_raw` in `rei_ext.h` (dual-form; slow path `rei_stage_raw_spill`
  in `stage_raw.c`): the core-owned raw-tier staging reservation — the
  RAWVEC / arena-RAWSPILL / region-RAWSPILL / flat-SHM_VEC cascade both
  first-party bindings staged by hand. One deliberate behavior delta
  against rei's former cascade: a channel SHM_VEC region-creation failure
  retries the arena before the reservation declines to the serialized
  tiers.
- The map morsel protocol in `rei_ext.h` (`morsel.c`, `rei_morsel_*`): the
  128-byte map header (one unified layout; bindings keep their magic tags),
  the generation-fenced CLAIM word protocol, AIMD batch sizing
  (`rei_morsel_sizer`), reset/re-arm, the abandon trim, the cancel word, and
  the lost-set scan — extracted from rei's and pyrei's map modules.

### Changed

- `rei_stage_fn` gains the binding ctx; `rei_exec_fn` receives a read
  ctx in place of the bare binding ctx.
