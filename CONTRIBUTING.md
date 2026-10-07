# Contributing

Contributions are welcome — bug reports and pull requests alike.
libmizu is pre-release and the API can change at any time, so open an issue to discuss larger changes before doing the work.

## Reporting bugs

Open a GitHub issue with a minimal reproducer, your platform and compiler, and the kernel version on Linux.
For a hang or a lost wakeup, name the transport (channel or pool) and the operation in flight — the wait paths are per-OS.

## Pull requests

Build and run the per-PR tiers before submitting:

```sh
make
make test               # unit + ext tiers
make test-integration   # forked-child tier
```

CI additionally runs the soak and fuzz tiers, sanitizer legs (ASan/UBSan/TSan), the export check, and the amalgamation smoke test.

Conventions:

- Commit messages are a single-line subject, no body.
- New behavior needs a test.
  Unit tests are in-process and deterministic, one binary per `.c` file; integration tests compile against `mizu.h` only, as the API's compile-time contract check.
- `include/mizu.h` is the pinned API contract.
  After an intended ABI change, regenerate the export list with `tools/check-exports.sh --write` — CI diffs the shared library's exported `mizu_*` symbols against it.
- Bump `MIZU_ABI_VERSION` on any wire-format change (the structs in `mizu.h`).
- `DESIGN.md` is the design authority: a change that alters an invariant updates it alongside.
- Keep the public headers' comments doxygen-formed (`/** ... */` blocks, `/**< ... */` trailing on members); `make docs` builds the API reference.
- Run `make tidy` after changing `src/`.
- Use clang-format for new code; the tree is not fully conformant, so no whole-tree reformats.

## Repository layout

- `include/mizu.h`: the stable consumer API.
  The soname tracks its ABI.
- `include/mizu_ext.h`: the binding-author tier, version-pinned per minor release.
- `src/`: the implementation.
  Every translation unit is platform-guarded internally, so all sources compile on every platform.
- `src/internal.h`: private internals; unit tests may include it, integration tests must not.
- `tests/unit/`, `tests/ext_surface.c`, `tests/integration/`, `tests/soak/`, `tests/fuzz/`: the test tiers.
- `bench/`: report-only microbenchmarks; records go in `bench/notes.md` with the commit SHA.
- `tools/`: the amalgamation, export-check, and Windows build scripts.
