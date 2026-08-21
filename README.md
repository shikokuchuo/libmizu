# librei れい

librei is a language-agnostic C11 core for lock-free shared-memory IPC.
It provides SPSC channels and work-stealing task pools over POSIX shm /
Win32 file mappings.

The first-party bindings are [rei](https://github.com/shikokuchuo/rei)
(R) and [pyrei](https://github.com/shikokuchuo/pyrei) (Python). You can
write bindings in other languages against the C ABI. Handles are
opaque, the surface is FFI-safe, and the library distributes as a
single-file amalgamation.

librei supports 64-bit platforms only. Linux requires kernel >= 5.3
(`pidfd_open`, no fallback).

## Status

Pre-release.

## License

MIT
