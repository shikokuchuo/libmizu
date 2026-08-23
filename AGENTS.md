# librei — project memory

C library for lock-free shared-memory IPC: SPSC channels and work-stealing
task pools behind a language-agnostic, FFI-safe C ABI. Pre-release (v0.1.0).
First-party bindings live in separate repos: `rei` (R) and `pyrei` (Python);
they vendor/compile these sources into their own modules.

## Requirements

- C11 compiler, 64-bit platform (wire formats need lock-free 64-bit atomics).
- Linux: kernel >= 5.3 (`pidfd_open`, no fallback).
- Windows: build with clang-cl only (MSVC C11 atomics unsupported).

## Layout

- `include/rei.h` — the public API contract. Documents ownership, threading,
  and `rei_status` returns per verb. The pinned API surface.
- `DESIGN.md` — design authority: invariants the implementation maintains
  (wire format, parker protocol, payload tiers, binding seam, retain table,
  zero-copy, crash atomicity, pool mechanics, ABI discipline). Code comments
  reference it.
- `src/` — implementation. Every TU is platform-guarded internally, so all
  sources compile on every platform (a foreign platform's file is empty).
  Platform waiters: `wait_linux.c` (futex), `wait_macos.c` (`__ulock`),
  `wait_win32.c` (WaitOnAddress + named events).
- `src/internal.h` — internals; unit tests may include it, integration tests
  must not.
- `tests/unit/` — in-process, deterministic tier (one binary per `.c` file).
- `tests/integration/` — real fork/spawn children; compiles against `rei.h`
  only, as the API's compile-time contract check.
- `tests/soak/` — minutes-long forked contention runs (nightly, not per-PR;
  `REI_SOAK_SECONDS` overrides the duration).
- `tests/fuzz/` — libFuzzer harnesses for the wire parsers a crashed peer
  can leave torn (preamble, pool header, REF/SHM_RAW identifier); built
  with clang, run as fixed-seed ASan+UBSan bursts.
- `bench/` — report-only microbenchmarks (no timing asserts); records are
  appended to `bench/notes.md` with the commit SHA.
- `tools/amalgamate.sh` — generates the two-file distribution `rei.c` + `rei.h`.
- `tools/build-win.bat` — Windows build (clang-cl).
- `dev/` — scratch copies of headers.

## Build and test

The Makefile is the only build system (on Unix):

```sh
make                  # librei.a + shared library
make test             # unit tier (per-PR)
make test-integration # fork/spawn tier
make test-soak        # soak tier (nightly; SOAK_SECONDS / REI_SOAK_SECONDS)
make test-fuzz        # fuzz bursts (clang; FUZZ_CC/FUZZ_SAN/FUZZ_RUNS/FUZZ_SEED)
make bench            # benchmark suite (report-only)
make coverage         # llvm-cov report over the unit tier (report-only)
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
  alignment, one shared hot word per cache line. `REI_ABI_VERSION` gates
  mixed builds; bump it on any wire-format change.
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
  `make test-integration`, and the amalgamation smoke test (compiles unit
  tests against `rei.c`, then links and runs a public-only program against
  `rei.c`/`rei.h` with no defines). A fuzz leg runs the fixed-seed bursts
  on ubuntu; sanitizer legs cover ASan/UBSan/TSan on the unit tier.
- `.github/workflows/release.yml` — on `v*` tags: checks tag matches
  `REI_VERSION`, generates the amalgamation for the release.

## Status

Region layer, parker, liveness lock, death listeners, the
spill/ledger/zc machinery (`src/spill.c`), the channel transport
(`src/channel.c`), the built-in bytes binding (`src/bytes.c`), and the
pool transport (`src/pool.c` — the full verb surface including the
vectored collects, introspection, map support, armed death watches, and
`rei_pool_task_release`, the finalizer release for uncollected task
handles) are complete and tested. All four test tiers are green: unit
(`tests/unit/`), integration (`tests/integration/`), soak
(`tests/soak/` — full-duplex channel and multi-worker pool contention),
and the fuzz bursts (`tests/fuzz/`). The bench suite's records live in
`bench/notes.md` (the 2026-08-23 entry is a full no-regression re-run:
all tiers, sanitizers, fuzz, and the amalgamation smoke green).
Concurrent region creation from
threads is a supported consumer pattern (`tests/unit/test_registry.c`).
License: MIT (`LICENSE.note` holds third-party RngStreams attribution).
