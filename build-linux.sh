#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ARCH="${1:-x64}"
CONFIG="${2:-debug}"

case "$ARCH:$CONFIG" in
    x64:debug) PRESET=linux-debug ;;
    x64:release) PRESET=linux-release ;;
    aarch64:debug) PRESET=linux-arm64-debug ;;
    aarch64:release) PRESET=linux-arm64-release ;;
    *) echo "Usage: $0 [x64|aarch64] [debug|release]" >&2; exit 2 ;;
esac

if [[ "$(uname -s)" != Linux ]]; then
    echo "Error: Linux builds must be run on Linux with this script." >&2
    exit 1
fi

if [[ -z "${VCPKG_ROOT:-}" ]]; then
    VCPKG_ROOT="$HOME/vcpkg"
    export VCPKG_ROOT
fi
if [[ ! -f "$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" ]]; then
    echo "Error: Set VCPKG_ROOT to a valid vcpkg checkout." >&2
    exit 1
fi
command -v cmake >/dev/null || { echo "Error: Install CMake first." >&2; exit 1; }

BUILD_DIR="$SCRIPT_DIR/out/build/$PRESET"
cmake --preset "$PRESET"
cmake --build "$BUILD_DIR" --parallel "${JOBS:-$(nproc 2>/dev/null || echo 2)}"
