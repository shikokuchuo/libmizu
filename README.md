# libmizu 水

[![CI](https://github.com/shikokuchuo/libmizu/actions/workflows/ci.yml/badge.svg)](https://github.com/shikokuchuo/libmizu/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

libmizu is a C library for lock-free shared-memory IPC.
It provides SPSC channels and work-stealing task pools through a language-agnostic C ABI.
Channels and pools run over POSIX shm or Win32 file mappings.

The first-party bindings are [mizu](https://github.com/shikokuchuo/mizu) (R) and [pymizu](https://github.com/shikokuchuo/pymizu) (Python).
You can write bindings in other languages against the C ABI.
Handles are opaque.
The surface is FFI-safe: every public operation is an exported function, with no macros or `static inline` in the public contract.
The library also distributes as a three-file amalgamation: `mizu.c`, `mizu.h`, and `mizu_ext.h`.

## API tiers

The headers are two deliberate tiers:

- **`mizu.h`** — the stable consumer API.
  The soname tracks its ABI; the stability promise starts at 1.0.
- **`mizu_ext.h`** — the binding-author API: the callback seam (`stage`, `read`, `exec`, ...), the stager/read/publish services, the built-in bytes binding, and promoted internals (parker, liveness, preamble, map support).
  It is version-pinned per minor release and may change without deprecation, ever — binding authors rebuild (or re-vendor) per minor release.

A third header, `src/internal.h`, is private and never installed; bindings never include it.

libmizu needs a C11 compiler and a 64-bit platform: the wire formats depend on lock-free 64-bit atomics.
On Linux, kernel 5.3 or later is required.

## Building

The Makefile is the only build system:

```sh
make                    # libmizu.a + the shared library
make test               # the unit tier (in-process, deterministic)
make test-integration   # the integration tier (forked child processes)
make test-soak          # the soak tier (minutes-long contention runs; nightly)
make test-fuzz          # libFuzzer bursts on the wire parsers (clang)
make bench              # the benchmark suite (report-only)
make coverage           # llvm-cov report over the unit tier (report-only)
make tidy               # clang-tidy over the library sources (report-only;
                        # TIDY overrides the binary)
make install            # honors PREFIX (/usr/local) and DESTDIR;
                        # also installs libmizu.pc for pkg-config
```

On Windows, run `tools\build-win.bat` instead.
This script uses clang-cl: MSVC C11 atomics are experimental and are not a supported target.
The script needs LLVM (clang-cl) and the Visual Studio C++ build tools.
GitHub Windows runners have both preinstalled.

Static builds need no defines: `MIZU_API` is empty by default.
This covers vendored and amalgamated builds: the first-party packages compile the sources into their own modules.
If you build libmizu as a shared library, define `MIZU_SHARED`.
If you compile the library itself on Windows, also define `MIZU_BUILDING`.
The Makefile and `build-win.bat` set these defines for you.

To generate the amalgamation, run:

```sh
tools/amalgamate.sh   # writes mizu.c + mizu.h + mizu_ext.h
```

## Quickstart

This example makes a channel between two processes.
Registering callbacks is binding-author surface, so it includes `mizu_ext.h`.
The host:

```c
#include <mizu_ext.h>   /* mizu_binding, mizu_binding_init (implies mizu.h) */

mizu_channel_opts opts;
mizu_channel_opts_init(&opts);
mizu_binding binding;
mizu_binding_init(&binding);
binding.stage = my_stage;   /* frame your objects as (hdr, payload) */
binding.read  = my_read;    /* the receive-side materializer */

mizu_channel *ch;
mizu_channel_create(&ch, &opts, &binding);

char token[64];
mizu_channel_token(ch, token, sizeof token);
/* spawn the peer however you like, passing the token */
mizu_channel_ready_wait(ch, -1);                 /* block until attach */

mizu_channel_send(ch, my_obj);
void *obj;
mizu_channel_recv(ch, &obj, -1);                 /* MIZU_OK / sentinels */
mizu_channel_close(ch, 5000);
```

The peer attaches with `mizu_channel_attach(&ch, token, &binding)`.
It reads the bootstrap payload with `mizu_channel_drop()` and signals `mizu_channel_ready_set()`.

[`include/mizu.h`](include/mizu.h) documents the full contract for each declaration: ownership, threading, and the `mizu_status` values that each verb returns.
[`DESIGN.md`](DESIGN.md) gives the invariants.

## Performance

Representative figures from the C bench suite (`make bench`), measured on an Apple M4 Pro (macOS, arm64).

| Benchmark | Best observed |
| --- | --- |
| Channel round trip (nil / 64 B payload) | 0.3–0.4 µs |
| Channel round trip (64 KiB, arena tier) | 4.0 µs |
| Channel round trip (1 MiB, spill tier) | 50 µs |
| Channel batch throughput (64 B, batches of 32) | 44 M msgs/s |
| Pool submit + collect (nil payload) | 0.4 µs |
| Pool submit + collect (4 KiB payload) | 1.0 µs |
| Pool task throughput (tiny echo tasks) | ~11 M tasks/s |

The full dated records, including parker wake latency and spill-region costs, live in [`bench/notes.md`](bench/notes.md).

## Status

Pre-release.

## License

MIT.
`LICENSE.note` has the third-party attribution (RngStreams jump matrices).
