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

First public release.

- Lock-free SPSC channels and work-stealing task pools over POSIX shared
  memory (Linux, macOS) and Win32 file mappings, behind a
  language-agnostic, FFI-safe C ABI: opaque handles, `mizu_status`
  terminal states instead of errors, vectored collects, batch submit,
  and pool introspection. 64-bit platforms; Linux needs kernel >= 5.3.
- Crash-atomic wire formats: every cross-process claim is copy-then-CAS,
  and the parker protocol (one epoch word per waiting entity over futex /
  `__ulock` / WaitOnAddress, plus named events) is the sole guarantee
  against lost wakeups.
- Process-liveness verdicts from a kernel-released exclusive lock per
  entity; per-platform death listeners (pidfd + epoll, dispatch sources,
  thread-pool waits) act as wake triggers only, and an orphan reaper
  reclaims dead creators' regions.
- Payload tiers: immediate NIL and single strings, raw vectors, a channel
  spill arena, named spill regions, and zero-copy layout regions
  (SHM_VEC) under a cross-process refcount — with a per-handle retain
  table, explicit release points, and churn-gated fallback to the copy
  tiers.
- The binding seam: the core moves opaque frames and never sees a
  language object. Bindings register `stage` / `read` / `exec` / `check`
  / `park` / `sweep` / `drop` callbacks per handle; staging is
  transactional, so a mid-stage abort leaves the handle consistent. A
  built-in bytes binding ships as the reference consumer.
- A three-tier API: the stable `mizu.h`, the binding-author `mizu_ext.h`
  (dual-form header inlines plus exported symbols, pinned per minor
  release), and private internals.
- The cross-language wire contract (normative in DESIGN.md, pinned by a
  byte-exact golden corpus): per-handle identity words (language plus
  append-only reader-capability bits), the 'I' interchange stream with a
  validating pull cursor and emit helpers, the err / task / ref tags,
  and the MIZH / MIZS / MIZL zero-copy layout formats.
- Cross-language pools: the neutral task stream with its bounded error
  envelope, task arguments by reference, the map descriptor and runner
  streams, and the morsel-claim protocol (`mizu_morsel_*`) — one header
  layout, generation-fenced claims, AIMD batch sizing, cancellation, and
  the lost-set scan.
- The Arrow frame writer: an Arrow C Data Interface → MIZL writer with
  dictionary (factor), temporal (Date / POSIXct / difftime, nanosecond
  verbatim) and uint64-verbatim forms, fixed-width conversion kernels,
  an inline small-table form, and per-column writers a binding's own
  emitters can share.
- Notify endpoints for event-driven bindings: named, edge-triggered
  wakes (a filesystem datagram socket on POSIX, an auto-reset event on
  Windows) signaled at the publish checkpoint; watches ride the
  death-listener machinery, and close and peer death also signal.
- L'Ecuyer-CMRG (MRG32k3a) stream jumping for deterministic per-element
  map seeds.
- Distribution as a three-file amalgamation (`mizu.c` + `mizu.h` +
  `mizu_ext.h`), `make install` with pkg-config, and a doxygen API
  reference.
