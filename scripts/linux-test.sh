#!/usr/bin/env bash
# Builds and tests the project inside a Linux container to exercise the epoll
# backend from a macOS development machine.
set -euo pipefail
cd "$(dirname "$0")/.."

IMAGE=seashield-linux-build
docker build -t "$IMAGE" -f scripts/Dockerfile.linux scripts

docker run --rm -v "$PWD":/src -w /src "$IMAGE" bash -ec '
  cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Debug
  cmake --build build-linux -j"$(nproc)"
  ctest --test-dir build-linux --output-on-failure'
