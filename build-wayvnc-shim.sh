#!/bin/bash
# Build libhaven_wayvnc_shim.so per-ABI against glibc and stage into
# core/local's assets dir for APK packaging.
#
# Why glibc (not the NDK / bionic):
#   The shim is loaded by `wayvnc` inside the proot rootfs (Arch / Alpine
#   / Debian). Those binaries link against glibc. A bionic-targeted .so
#   has DT_NEEDED libdl.so / libc.so which glibc cannot resolve.
#
# Required toolchains (typically already on F-Droid's Debian buildserver
# and on most Linux dev machines):
#   - gcc-aarch64-linux-gnu  (for arm64-v8a)
#   - gcc-x86-64-linux-gnu   (for x86_64)
#
# Usage:
#   ./build-wayvnc-shim.sh            # builds both ABIs
#   ./build-wayvnc-shim.sh arm64-v8a  # arm64 only
#   ./build-wayvnc-shim.sh x86_64     # x86_64 only

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SRC="$SCRIPT_DIR/wayvnc-shim/libhaven_wayvnc_shim.c"
ASSET_ROOT="$REPO_ROOT/core/local/src/main/assets/wayvnc-shim"

build_one() {
    local abi="$1"
    local cc
    case "$abi" in
        arm64-v8a) cc="aarch64-linux-gnu-gcc" ;;
        x86_64)    cc="x86_64-linux-gnu-gcc" ;;
        *) echo "unsupported ABI: $abi" >&2; exit 1 ;;
    esac
    command -v "$cc" >/dev/null 2>&1 || {
        echo "$cc not found — install gcc-aarch64-linux-gnu / gcc-x86-64-linux-gnu" >&2
        exit 1
    }
    local out_dir="$ASSET_ROOT/$abi"
    local out="$out_dir/libhaven_wayvnc_shim.so"
    mkdir -p "$out_dir"
    echo "=== Building wayvnc shim for $abi ==="
    "$cc" -shared -fPIC -O2 -Wall -Wextra -o "$out" "$SRC"
    case "$abi" in
        arm64-v8a) command -v aarch64-linux-gnu-strip >/dev/null && aarch64-linux-gnu-strip "$out" || true ;;
        x86_64) command -v x86_64-linux-gnu-strip >/dev/null && x86_64-linux-gnu-strip "$out" || true ;;
    esac
    ls -la "$out"
}

if [ $# -eq 0 ]; then
    build_one arm64-v8a
    build_one x86_64
else
    build_one "$1"
fi
