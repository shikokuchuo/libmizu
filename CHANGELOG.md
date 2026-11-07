# Changelog

All notable changes to libmizu are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versions carry an ABI contract: any wire-format change bumps
`MIZU_ABI_VERSION` in `include/mizu.h`, and the soname tracks
`MIZU_VERSION_MAJOR`.

## [Unreleased]

### Changed

- Faster masked conversions of int32 and int64 Arrow columns.

### Fixed

- An event-driven pool consumer could miss the wake for a task result
  published immediately after submission and wait indefinitely.

## [0.1.0] - 2026-11-04

### Added

First public release.

- Lock-free SPSC channels and work-stealing task pools over POSIX shared
  memory (Linux, macOS) and Win32 file mappings, behind a
  language-agnostic, FFI-safe C ABI: opaque handles, `mizu_status`
  terminal states, vectored collects, batch submit, and pool
  introspection. 64-bit platforms; Linux needs kernel >= 5.3.
- Crash-atomic wire formats: every cross-process claim is copy-then-CAS,
  so a peer dying mid-operation cannot wedge a queue, and a per-waiter
  parker protocol (futex / `__ulock` / WaitOnAddress) guarantees no lost
  wakeups.
- Process-liveness verdicts from a kernel-released exclusive lock per
  entity, with per-platform death listeners and an orphan reaper
  reclaiming dead creators' regions.
- Payload tiers from immediate values to spill regions and zero-copy
  layout regions (SHM_VEC) under a cross-process refcount, with
  churn-gated fallback to the copy tiers.
- A binding seam where the core moves opaque frames and never sees a
  language object: bindings register `stage` / `read` / `exec` / `check`
  / `park` / `sweep` / `drop` callbacks per handle, and staging is
  transactional. A built-in bytes binding ships as the reference
  consumer.
- A three-tier API: the stable `mizu.h`, the binding-author `mizu_ext.h`
  (dual-form header inlines plus exported symbols, pinned per minor
  release), and private internals.
- The cross-language wire contract, normative in DESIGN.md and pinned by
  a byte-exact golden corpus: per-handle identity words with append-only
  capability bits, the 'I' interchange stream, and the MIZH / MIZS /
  MIZL zero-copy layout formats.
- Cross-language pools: a neutral task stream with a bounded error
  envelope, task arguments by reference, and the morsel-claim map
  protocol (`mizu_morsel_*`) for self-scheduling runners.
- An Arrow C Data Interface → MIZL frame writer with dictionary,
  temporal and uint64-verbatim forms, fixed-width conversion kernels, an
  inline small-table form, and per-column writers a binding's own
  emitters can share.
- Named, edge-triggered notify endpoints for event-driven bindings,
  signaled at the publish checkpoint; close and peer death also signal.
- L'Ecuyer-CMRG (MRG32k3a) stream jumping for deterministic per-element
  map seeds.
- Distribution as a four-file amalgamation (`mizu.c` + `mizu.h` +
  `mizu_ext.h` + `mizu_ext.c`), `make install` with pkg-config, and a
  doxygen API reference.
