# libmizu 水

[![CI](https://github.com/shikokuchuo/libmizu/actions/workflows/ci.yml/badge.svg)](https://github.com/shikokuchuo/libmizu/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

libmizu is a C library for lock-free shared-memory IPC.

libmizu is for runtimes that parallelize with processes rather than threads.
It makes the communication between those processes fast enough to divide work at thread granularity.

libmizu uses shared memory as the fastest IPC transport.
Processes read and write into shared memory directly, with no kernel involvement and no data copies through system sockets.
This means round trips drop from tens of microseconds over sockets to under a microsecond (see [Performance](#performance)).

Channels and work-stealing task pools run over POSIX shared memory (Linux, macOS) or Win32 file mappings (Windows).
The hot path stays in user space.
Data moves through single-producer single-consumer rings with batched publication.
Waiting is spin-then-park, and peer death arrives as an OS event.

Bindings share one wire format, so processes in different languages can exchange data over the same channel.
The first-party bindings are [mizu](https://github.com/shikokuchuo/mizu) for R and [pymizu](https://github.com/shikokuchuo/pymizu) for Python.
[Writing a binding](#writing-a-binding) covers other languages.

- Lock-free SPSC rings and work-stealing deques over shared memory
- Bounded, microsecond-latency waiting: futex, `__ulock`, or `WaitOnAddress` per platform
- Peer crashes detected at OS notification latency — kernel-released locks, no heartbeats or polling
- Crash-atomic claims (copy-then-CAS): a dead peer never wedges a queue
- FFI-safe ABI: opaque handles, exported functions only, `mizu_status` sentinels on hot paths
- C11, 64-bit: Linux, macOS, and Windows

> **Pre-release.** The API is not stable and can change at any time before a release.

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

The full dated records, including parker wake latency and spill-region costs, live in [`bench/notes.md`](https://github.com/shikokuchuo/libmizu/blob/main/bench/notes.md).

## Quickstart

This example makes a channel between two processes with the built-in bytes binding.
The bytes binding needs no callbacks, so the example compiles as written.
The host:

```c
#include <mizu_ext.h>   /* mizu_binding_bytes, mizu_bytes (implies mizu.h) */

mizu_channel_opts opts;
mizu_channel_opts_init(&opts);
mizu_binding binding;
mizu_binding_bytes(&binding);

mizu_channel *ch;
mizu_channel_create(&ch, &opts, &binding);

char token[64];
mizu_channel_token(ch, token, sizeof token);
/* spawn the peer however you like, passing the token */
mizu_channel_ready_wait(ch, -1);                 /* block until attach */

mizu_bytes out = { "hello", 5 };                 /* { data, len } */
mizu_channel_send(ch, &out);

void *obj;
mizu_channel_recv(ch, &obj, -1);                 /* MIZU_OK / sentinels */
mizu_bytes *in = obj;                            /* in->data, in->len */
mizu_bytes_free(in);

mizu_channel_close(ch, 5000);                    /* up to 5 s for the peer's close */
```

The peer fills its binding with `mizu_binding_bytes()` too and attaches with `mizu_channel_attach(&ch, token, &binding)`.
It reads the bootstrap payload with `mizu_channel_drop()` and signals `mizu_channel_ready_set()`.

To move a language's own objects rather than byte buffers, register callbacks on the binding instead — the binding-author seam covered in [Writing a binding](#writing-a-binding):

```c
mizu_binding binding;
mizu_binding_init(&binding);
binding.stage = my_stage;   /* frame your objects as (hdr, payload) */
binding.read  = my_read;    /* the receive-side materializer */
```

[`include/mizu.h`](include/mizu.h) documents the full contract for each declaration: ownership, threading, and the `mizu_status` values that each verb returns.
[`DESIGN.md`](DESIGN.md) gives the invariants.
The rendered API reference lives at [shikokuchuo.net/libmizu](https://shikokuchuo.net/libmizu/).

## Writing a binding

A binding teaches libmizu to turn the language's values into frames and back.
The `stage` and `read` callbacks from the quickstart do the conversion, and `exec` runs tasks on pool workers.
libmizu does the rest: the rings, waiting and waking, peer-death detection, and work-stealing.
To start without callbacks, the built-in bytes binding — `mizu_binding_bytes()` — sends and receives byte buffers, as the quickstart shows.

Every operation is an exported function.
A language with a foreign function interface can call libmizu from the shared library, with no C of its own.
Every handle is opaque, so libmizu's internals can change without breaking the binding.
Bindings that compile C can add the amalgamation to their own build instead: `tools/amalgamate.sh` writes `mizu.c`, `mizu.h`, and `mizu_ext.h`.

## API tiers

The headers form two tiers:

- **`mizu.h`** — the stable consumer API.
  The soname tracks its ABI. The stability promise starts at 1.0.
- **`mizu_ext.h`** — the binding-author API: the callback seam (`stage`, `read`, `exec`, ...), the stager/read/publish services, the built-in bytes binding, and promoted internals (parker, liveness, preamble, map support).
  It is version-pinned per minor release and can change without deprecation. Binding authors rebuild (or re-vendor) per minor release.

A third header, `src/internal.h`, is private. It is never installed, and bindings never include it.

## Building

libmizu needs a C11 compiler and a 64-bit platform: the wire formats depend on lock-free 64-bit atomics.
On Linux, kernel 5.3 or later is required.

The Makefile is the only build system:

```sh
make                    # libmizu.a + the shared library
make test               # the unit and ext tiers (in-process, deterministic)
make test-integration   # the integration tier (forked child processes)
make test-soak          # the soak tier (minutes-long contention runs; nightly)
make test-fuzz          # libFuzzer bursts on the wire parsers (clang)
make bench              # the benchmark suite (report-only)
make coverage           # llvm-cov report over the unit tier (report-only)
make tidy               # clang-tidy over the library sources (report-only;
                        # TIDY overrides the binary)
make docs               # doxygen API reference into docs/html
make install            # honors PREFIX (/usr/local) and DESTDIR;
                        # also installs libmizu.pc for pkg-config
```

On Windows, run `tools\build-win.bat` instead.
The script builds with clang-cl because MSVC C11 atomics are experimental, and it needs LLVM (clang-cl) and the Visual Studio C++ build tools.
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

## Contributing

The repository layout and contributor notes live in [CONTRIBUTING.md](CONTRIBUTING.md).

## License

MIT.
`LICENSE.note` has the third-party attributions (RngStreams jump matrices, vendored doxygen stylesheet).
