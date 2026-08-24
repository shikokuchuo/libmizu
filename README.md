# librei れい

[![CI](https://github.com/shikokuchuo/librei/actions/workflows/ci.yml/badge.svg)](https://github.com/shikokuchuo/librei/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

librei is a C library for lock-free shared-memory IPC.
It provides SPSC channels and work-stealing task pools through a language-agnostic C ABI.
Channels and pools run over POSIX shm or Win32 file mappings.

The first-party bindings are [rei](https://github.com/shikokuchuo/rei) (R) and [pyrei](https://github.com/shikokuchuo/pyrei) (Python).
You can write bindings in other languages against the C ABI.
Handles are opaque.
The surface is FFI-safe: every public operation is an exported function, with no macros or `static inline` in the public contract.
The library also distributes as a two-file amalgamation: `rei.c` and `rei.h`.

librei needs a C11 compiler and a 64-bit platform: the wire formats depend on lock-free 64-bit atomics.
On Linux, kernel 5.3 or later is required.

## Building

The Makefile is the only build system:

```sh
make                    # librei.a + the shared library
make test               # the unit tier (in-process, deterministic)
make test-integration   # the integration tier (forked child processes)
make test-soak          # the soak tier (minutes-long contention runs; nightly)
make test-fuzz          # libFuzzer bursts on the wire parsers (clang)
make bench              # the benchmark suite (report-only)
make coverage           # llvm-cov report over the unit tier (report-only)
make tidy               # clang-tidy over the library sources (report-only;
                        # TIDY overrides the binary)
make install            # honors PREFIX (/usr/local) and DESTDIR;
                        # also installs librei.pc for pkg-config
```

On Windows, run `tools\build-win.bat` instead.
This script uses clang-cl: MSVC C11 atomics are experimental and are not a supported target.
The script needs LLVM (clang-cl) and the Visual Studio C++ build tools.
GitHub Windows runners have both preinstalled.

Static builds need no defines: `REI_API` is empty by default.
This covers vendored and amalgamated builds: the first-party packages compile the sources into their own modules.
If you build librei as a shared library, define `REI_SHARED`.
If you compile the library itself on Windows, also define `REI_BUILDING`.
The Makefile and `build-win.bat` set these defines for you.

To generate the amalgamation, run:

```sh
tools/amalgamate.sh   # writes rei.c + rei.h
```

## Quickstart

This example makes a channel between two processes.
The host:

```c
rei_channel_opts opts;
rei_channel_opts_init(&opts);
rei_binding binding;
rei_binding_init(&binding);
binding.stage = my_stage;   /* frame your objects as (hdr, payload) */
binding.read  = my_read;    /* the receive-side materializer */

rei_channel *ch;
rei_channel_create(&ch, &opts, &binding);

char token[64];
rei_channel_token(ch, token, sizeof token);
/* spawn the peer however you like, passing the token */
rei_channel_ready_wait(ch, -1);                 /* block until attach */

rei_channel_send(ch, my_obj);
void *obj;
rei_channel_recv(ch, &obj, -1);                 /* REI_OK / sentinels */
rei_channel_close(ch, 5000);
```

The peer attaches with `rei_channel_attach(&ch, token, &binding)`.
It reads the bootstrap payload with `rei_channel_drop()` and signals `rei_channel_ready_set()`.

[`include/rei.h`](include/rei.h) documents the full contract for each declaration: ownership, threading, and the `rei_status` values that each verb returns.
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
