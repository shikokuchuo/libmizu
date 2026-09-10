# Changelog

All notable changes to librei are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versions carry an ABI contract: any wire-format change bumps
`REI_ABI_VERSION` in `include/rei.h`, and the soname tracks
`REI_VERSION_MAJOR`.

## [Unreleased]

Pre-release; no tagged version yet. On the first tag, rename this section
to `## [0.1.0] - <date>` and start a fresh `Unreleased` section above it.

### Added

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
- `rei_handle_binding_ctx` handle query in `rei_ext.h`: the registered
  binding ctx, for the stage hook (which receives the handle, not the ctx).
