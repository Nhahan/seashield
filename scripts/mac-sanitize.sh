#!/usr/bin/env bash
# Run the ASan / TSan test matrix locally on macOS using Homebrew LLVM.
#
#   ./scripts/mac-sanitize.sh [address|thread|all]   (default: all)
#
# Why not the default (Apple) clang: the sanitizer runtimes shipped with
# Xcode 26 are broken on this macOS — TSan SIGSEGVs at init inside
# libclang_rt.tsan_osx_dynamic.dylib (__tsan::SlotLock) and ASan hangs in a
# spin loop, even on trivial single-threaded tests. Both are runtime bugs, not
# project code: the same sources pass ASan+TSan 213/213 on (a) Homebrew LLVM
# locally and (b) CI's ubuntu-24.04 AND macos-14 sanitizer jobs. Homebrew
# LLVM ships a mainline compiler-rt that works, so point CMake at it.
#
# CI remains the authority (ci.yml `sanitizers` job, both OSes); this script is
# the local fast path while Apple's runtime is broken.
set -euo pipefail
cd "$(dirname "$0")/.."

LLVM="$(brew --prefix llvm 2>/dev/null)/bin"
[ -x "$LLVM/clang++" ] || { echo "Homebrew LLVM not found — 'brew install llvm'"; exit 1; }

run() { # preset
  local preset="$1" dir="build-$1-llvm"
  echo "=== $preset via Homebrew LLVM ==="
  cmake -S . -B "$dir" \
    -DCMAKE_C_COMPILER="$LLVM/clang" -DCMAKE_CXX_COMPILER="$LLVM/clang++" \
    -DSEASHIELD_SANITIZE="$preset" >/dev/null
  cmake --build "$dir" -j"$(sysctl -n hw.ncpu)" >/dev/null
  # --timeout guards against a hung test wedging the run for hours.
  ctest --test-dir "$dir" --timeout 180
}

case "${1:-all}" in
  address) run address ;;
  thread)  run thread ;;
  all)     run address; run thread ;;
  *) echo "usage: $0 [address|thread|all]"; exit 2 ;;
esac
