#!/bin/sh
# Verifies the shared library exports exactly the mizu.h + mizu_ext.h
# surface: no leaks, no missing symbols (the latter is an ABI break).
# Expects the library to be built already (run `make` first).
set -eu

cd "$(dirname "$0")/.."

case "$(uname -s)" in
  Darwin)
    lib=$(ls libmizu.*.dylib)
    nm -gU "$lib" | awk '{print $NF}' | sed -n 's/^_\(mizu_.*\)/\1/p' | sort -u \
      > /tmp/mizu-exports.actual
    ;;
  Linux)
    lib=$(ls libmizu.so.*.*.*)
    nm -D --defined-only "$lib" | awk '{print $NF}' | grep '^mizu_' | sort -u \
      > /tmp/mizu-exports.actual
    ;;
  *)
    echo "check-exports: unsupported platform" >&2
    exit 1
    ;;
esac

if [ "${1:-}" = "--write" ]; then
  cp /tmp/mizu-exports.actual tools/exports.txt
  echo "check-exports: rewrote tools/exports.txt"
  exit 0
fi

if ! diff -u tools/exports.txt /tmp/mizu-exports.actual; then
  echo "check-exports: export surface drifted from tools/exports.txt" >&2
  echo "If the change is intended, regenerate: tools/check-exports.sh --write" >&2
  exit 1
fi
echo "check-exports: OK ($(wc -l < tools/exports.txt | tr -d ' ') symbols)"
