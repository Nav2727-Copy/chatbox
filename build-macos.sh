#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ARCH="${1:-arm64}"
CONFIG="${2:-debug}"

case "$ARCH:$CONFIG" in
    arm64:debug) PRESET=macos-arm64-debug ;;
    native:debug) PRESET=macos-debug ;;
    *) echo "Usage: $0 [arm64|native] [debug]" >&2; exit 2 ;;
esac

if [[ "$(uname -s)" != Darwin ]]; then
    echo "Error: macOS builds must be run on macOS with this script." >&2
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
cmake --build "$BUILD_DIR" --parallel "${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 2)}"
