#!/usr/bin/env bash
# scripts/build.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.."&& pwd)"
cd "$ROOT"

BUILD_DIR="$ROOT/build"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake .. -G "Ninja" -DCMAKE_BUILD_TYPE=Release
cmake --build . -- -j2

