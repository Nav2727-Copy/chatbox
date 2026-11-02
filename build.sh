#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PRESET="${1:-linux-debug}"
BUILD_DIR="$SCRIPT_DIR/out/build/$PRESET"

if ! command -v cmake >/dev/null 2>&1; then
    echo "Error: CMake is required. Install CMake and a C++ compiler first." >&2
    exit 1
fi

if [[ -z "${VCPKG_ROOT:-}" ]]; then
    if [[ -f "$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake" ]]; then
        export VCPKG_ROOT="$HOME/vcpkg"
    else
        echo "Error: vcpkg was not found. Set VCPKG_ROOT to your vcpkg directory." >&2
        exit 1
    fi
fi

if [[ ! -f "$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" ]]; then
    echo "Error: VCPKG_ROOT does not contain scripts/buildsystems/vcpkg.cmake: $VCPKG_ROOT" >&2
    exit 1
fi

cmake --preset "$PRESET"
cmake --build "$BUILD_DIR" --parallel "${JOBS:-$(nproc 2>/dev/null || echo 2)}"
