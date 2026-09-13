# librei — project memory

C library for lock-free shared-memory IPC: SPSC channels and work-stealing
task pools behind a language-agnostic, FFI-safe C ABI. Pre-release (v0.1.0).
First-class bindings live in sibling repos: `rei` (R)
and `pyrei` (Python); they vendor/compile these sources into their own
packages. The channel
and pool transports plus the built-in bytes binding are complete and
tested. License: MIT (`LICENSE.note` holds third-party RngStreams
attribution).

## Requirements

- C11 compiler, 64-bit platform (wire formats need lock-free 64-bit atomics).
- Linux: kernel >= 5.3 (`pidfd_open`, no fallback).
- Windows: build with clang-cl only (MSVC C11 atomics unsupported).

## Layout

- `include/rei.h` — the public API contract. Documents ownership, threading,
  and `rei_status` returns per verb. The pinned API surface.
- `include/rei_ext.h` — dual-form accessors over the wire structs: C
  consumers get `static inline`s, FFI consumers bind the exported symbols
  from `src/ext.c` (`REI_EXT_NO_INLINES` keeps the inlines out of that TU).
- `CHANGELOG.md` — Keep a Changelog; versions carry the ABI contract.
- `DESIGN.md` — design authority: invariants the implementation maintains
  (wire format, parker protocol, payload tiers, binding seam, retain table,
  zero-copy, crash atomicity, pool mechanics, ABI discipline). Code comments
  reference it.
- `src/` — implementation. Every TU is platform-guarded internally, so all
  sources compile on every platform (a foreign platform's file is empty).
  Main pieces: region layer (`shm.c`, `shm_rw.c`, `preamble.c`,
  `err_tls.c` — the thread-local error slot for handle-free entry points;
  `shm.c` + `err_tls.c` vendor as a unit and depend only on `rei.h`),
  parker (`parker.c`), liveness lock (`liveness.c`), spill/ledger/zc
  machinery (`spill.c`), channel transport (`channel.c`), bytes binding
  (`bytes.c`), pool transport (`pool.c` — full verb surface: vectored
  collects, introspection, map support, armed death watches,
  `rei_pool_task_release`, `rei_pool_submit_batch_fn`), the raw-tier
  staging reservation (`stage_raw.c` — `rei_stage_raw` /
  `rei_stage_raw_spill`), and the map morsel-claim protocol (`morsel.c` —
  `rei_morsel_*`). Also: `ext.c`
  (exported half of `rei_ext.h`; excluded from the amalgamation, which
  already carries the inlines), `err.c` (handle-bound error recorders),
  `rng_jump.c` (L'Ecuyer MRG32k3a stream jumping, for a future rei_map),
  `tune.c` (`rei_tune` — glibc malloc thresholds, called by the binding
  at load; GLIBC_TUNABLES wins).
  Platform waiters: `wait_linux.c` (futex), `wait_macos.c` (`__ulock`),
  `wait_win32.c` (WaitOnAddress + named events).
  Performance notes: the parker caches the self pid with an atfork reset;
  channel/pool reads start from a per-handle read-ctx template; the error
  recorders are marked cold; reused spill regions collapse to huge pages
  at free-list insert (Linux THP), so only regions that completed a
  consumer-done cycle pay for it.
- `src/internal.h` — internals; unit tests may include it, integration tests
  must not.
- `tests/unit/` — in-process, deterministic tier (one binary per `.c` file).
- `tests/ext_surface.c` — ext tier: includes only `rei.h` + `rei_ext.h`,
  references every ext symbol and runs functional checks. Default mode
  exercises the header inlines (also compiled against the amalgamation in
  CI); `EXT_PROBE_EXPORTS` mode (`make test-ext`) forces the exported
  symbols to link, proving both forms agree.
- `tests/integration/` — real fork/spawn children; compiles against `rei.h`
  only, as the API's compile-time contract check.
- `tests/soak/` — minutes-long forked contention runs (nightly, not per-PR;
  `REI_SOAK_SECONDS` overrides the duration).
- `tests/fuzz/` — libFuzzer harnesses for the wire parsers a crashed peer
  can leave torn (preamble, pool header, REF/SHM_RAW identifier); built
  with clang, run as fixed-seed ASan+UBSan bursts.
- `bench/` — report-only microbenchmarks (no timing asserts); records are
  appended to `bench/notes.md` with the commit SHA.
- `librei.pc.in` — pkg-config template; `make install` sed-substitutes it
  into `$(PREFIX)/lib/pkgconfig/librei.pc`.
- `.clang-tidy` / `.clang-format` / `.editorconfig` — diagnostics and style
  configs. clang-format matches the existing style but the tree isn't fully
  conformant; use it for new code, no whole-tree reformat, no CI gate.
- `tools/amalgamate.sh` — generates the two-file distribution `rei.c` + `rei.h`.
- `tools/check-exports.sh` — diffs the shared library's exported `rei_*`
  symbols against `tools/exports.txt` (no leaks, no missing = ABI break);
  `--write` regenerates after an intended change. Run in CI.
- `tools/build-win.bat` — Windows build (clang-cl).
- `dev/` — scratch copies of headers.

## Build and test

The Makefile is the only build system (on Unix):

```sh
make                  # librei.a + shared library
make test             # unit + ext tiers (per-PR)
make test-unit        # unit tier only
make test-ext         # ext surface tier (EXT_PROBE_EXPORTS mode)
make test-integration # fork/spawn tier
make test-soak        # soak tier (nightly; SOAK_SECONDS / REI_SOAK_SECONDS)
make test-fuzz        # fuzz bursts (clang; FUZZ_CC/FUZZ_SAN/FUZZ_RUNS/FUZZ_SEED)
make bench            # benchmark suite (report-only)
make coverage         # llvm-cov report over the unit tier (report-only)
make tidy             # clang-tidy over src/ (report-only; TIDY overrides the
                      # binary, e.g. TIDY=/opt/homebrew/opt/llvm/bin/clang-tidy).
                      # Run this after changing src/.
make compile_commands.json  # clangd compilation database (gitignored;
                            # regenerates when the Makefile changes)
make install          # honors PREFIX (/usr/local) and DESTDIR
tools/amalgamate.sh   # writes rei.c + rei.h
```

Compile flags: `-std=c11 -Wall -Wextra -Wpedantic -Werror -fvisibility=hidden`.

Local caveats on this Mac: the fuzz tier needs Homebrew LLVM —
`FUZZ_CC=/opt/homebrew/opt/llvm/bin/clang` (Xcode clang lacks the
libFuzzer runtime) and `FUZZ_SAN="-fsanitize=fuzzer,undefined"` (Homebrew
LLVM's ASan hangs at process startup here; CI runs the full trio).
Sanitizer overrides via CFLAGS/LDFLAGS cover unit + soak + bench; the
integration rule compiles plain against `rei.h` only by design (the
consumer contract check), so it takes no sanitizer flags.

Defines: none for static/vendored/amalgamated builds (`REI_API` empty);
`REI_SHARED` when building/using the shared library; also `REI_BUILDING` when
compiling the library itself on Windows. The Makefile and build-win.bat set
these.

## Conventions

- Handles are opaque; nothing public passes or returns a struct by value;
  extensible structs carry a size field; every public op is an exported
  function (no macros or `static inline` in the public contract).
- Terminal transport states are `rei_status` values (`REI_FULL`,
  `REI_TIMEOUT`, `REI_CLOSED`, `REI_PEER_GONE`), not errors. `REI_ERR` is a
  real error with a portable `rei_errcat` category plus message on the handle.
- Structs in `rei.h` are the wire format: sizes static-asserted, 64-byte
  alignment, one shared hot word per cache line. `rei_task` packs into 8
  bytes. `REI_ABI_VERSION` gates mixed builds; bump it on any
  wire-format change.
- Concurrent region creation from threads is a supported consumer pattern
  (`tests/unit/test_registry.c`).
- The core moves opaque frames and never sees a language object; bindings
  register callbacks (`stage`, `read`, `exec`, `check`, `park`, `sweep`,
  `drop`) at create/attach/join. Staging is transactional — a binding may
  abandon mid-stage (R longjmp) and leave the handle consistent.
- Soname tracks `REI_VERSION_MAJOR`; release tags (`v*`) must match the
  `REI_VERSION_*` macros in `include/rei.h` (enforced by release CI).
- Comments are concise and to the point: state the invariant or the
  why (protocol guarantees, platform quirks), skip narrative filler and
  restatement of the code. Keep license/attribution headers intact.
- Commit messages are a single line (subject only, no body).
- Never push without explicit approval — every push must be approved by
  the user first.

## CI

- `.github/workflows/ci.yml` — matrix: ubuntu-latest, ubuntu-24.04-arm,
  macos-latest, macos-15-intel. Runs `make`, `make test`,
  `make test-integration`, `tools/check-exports.sh`, and the amalgamation
  smoke test (compiles unit tests against `rei.c`, then links and runs a
  public-only program plus `tests/ext_surface.c` against `rei.c`/`rei.h`
  with no defines). A fuzz leg runs the fixed-seed bursts
  on ubuntu; sanitizer legs cover ASan/UBSan/TSan on the unit tier.
- `.github/workflows/release.yml` — on `v*` tags: checks tag matches
  `REI_VERSION`, generates the amalgamation for the release.
