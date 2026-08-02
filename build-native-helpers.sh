#!/bin/bash
# Build the Android-side native helpers that ship in core/wayland's jniLibs.
#
# Why this exists: F-Droid's recipe `scandelete`s core/wayland/src/main/jniLibs
# and then rebuilt only liblabwc_android.so, so every other binary in that
# directory was deleted and never regenerated — F-Droid's APK shipped without
# the GPU-acceleration renderer, the XWayland wrapper and the GLES benchmark,
# while ours shipped the committed copies. Nothing built these from source at
# all; they had been compiled by hand. Same failure shape as #469, one layer up.
#
# All four are ELF *executables* renamed lib*.so. Android's installer extracts
# anything matching lib*.so from an APK into nativeLibraryDir, which is the only
# place an app may exec from under W^X — the same trick build-proot/build.sh and
# build-ffmpeg/build.sh use. They are NOT shared libraries; do not dlopen them.
#
# Targets bionic via the NDK (unlike build-wayvnc-shim.sh, which targets glibc
# because that shim is LD_PRELOADed inside the proot rootfs). These run as
# Android processes and link libEGL/libandroid, so they must be NDK builds.
#
# Usage:
#   ./build-native-helpers.sh                 # arm64-v8a, all helpers
#   ABI=x86_64 ./build-native-helpers.sh      # another ABI
#   ./build-native-helpers.sh xwayland_wrapper benchmark_gles   # a subset

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

ABI="${ABI:-arm64-v8a}"
API="${API:-26}"          # matches core/wayland minSdk
OUT_DIR="$SCRIPT_DIR/jniLibs/$ABI"

case "$ABI" in
    arm64-v8a)   TRIPLE=aarch64-linux-android ;;
    x86_64)      TRIPLE=x86_64-linux-android ;;
    armeabi-v7a) TRIPLE=armv7a-linux-androideabi ;;
    *) echo "unsupported ABI: $ABI" >&2; exit 1 ;;
esac

# NDK discovery mirrors build-proot/build.sh: explicit env wins, else newest
# installed. F-Droid sets ANDROID_NDK_HOME from the recipe's `ndk:` key.
NDK="${ANDROID_NDK_HOME:-}"
if [ -z "$NDK" ] && [ -n "${ANDROID_SDK_ROOT:-}${ANDROID_HOME:-}" ]; then
    for sdk in "${ANDROID_SDK_ROOT:-}" "${ANDROID_HOME:-}"; do
        [ -d "$sdk/ndk" ] || continue
        NDK="$(find "$sdk/ndk" -maxdepth 1 -mindepth 1 -type d | sort -V | tail -1)"
        [ -n "$NDK" ] && break
    done
fi
[ -n "$NDK" ] && [ -d "$NDK" ] || { echo "Android NDK not found — set ANDROID_NDK_HOME" >&2; exit 1; }

TC="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
CC="$TC/${TRIPLE}${API}-clang"
[ -x "$CC" ] || { echo "no NDK clang at $CC" >&2; exit 1; }

mkdir -p "$OUT_DIR"
echo "=== wayland-android native helpers: ABI=$ABI API=$API NDK=$NDK ==="

# -pie is the default for API 21+, but state it: these must be PIE executables,
# and a non-PIE binary is refused outright by Android's linker.
COMMON_CFLAGS=(-O2 -fPIE -pie -Wall -Wextra)

build_xwayland_wrapper() {
    echo "--- libxwayland_wrapper.so ---"
    "$CC" "${COMMON_CFLAGS[@]}" \
        -o "$OUT_DIR/libxwayland_wrapper.so" \
        "$SCRIPT_DIR/xwayland_wrapper.c" \
        -llog
}

build_benchmark_gles() {
    echo "--- libbenchmark_gles.so ---"
    # Needs libwayland-client, which build-android.sh cross-builds into
    # sysroot/$ABI. Link it statically: the benchmark runs as an Android
    # process and there is no libwayland-client.so in nativeLibraryDir to
    # resolve against at runtime — which is why the committed binary lists
    # no wayland NEEDED entry despite using the API.
    local sysroot="$SCRIPT_DIR/sysroot/$ABI"
    # libwayland-client's wl_closure_invoke dispatches through libffi, so the
    # static link needs libffi.a alongside it.
    local libwl="$sysroot/lib/libwayland-client.a"
    if [ ! -f "$libwl" ]; then
        echo "ERROR: $libwl missing — run build-android.sh (or build_liblabwc_android.sh) for $ABI first" >&2
        exit 1
    fi
    # xdg-shell-protocol.c and its header are pre-generated and committed next
    # to the client, so this needs no wayland-scanner on the build machine.
    "$CC" "${COMMON_CFLAGS[@]}" \
        -I"$SCRIPT_DIR/benchmark" -I"$sysroot/include" \
        -o "$OUT_DIR/libbenchmark_gles.so" \
        "$SCRIPT_DIR/benchmark/benchmark_client.c" \
        "$SCRIPT_DIR/benchmark/xdg-shell-protocol.c" \
        "$libwl" "$sysroot/lib/libffi.a" \
        -lEGL -lGLESv2 -llog -landroid -lnativewindow -lm
}

TARGETS=("$@")
[ ${#TARGETS[@]} -eq 0 ] && TARGETS=(xwayland_wrapper benchmark_gles)

for t in "${TARGETS[@]}"; do
    case "$t" in
        xwayland_wrapper) build_xwayland_wrapper ;;
        benchmark_gles)   build_benchmark_gles ;;
        *) echo "unknown target: $t" >&2; exit 1 ;;
    esac
done

echo "=== built into $OUT_DIR ==="
ls -la "$OUT_DIR" | grep -E 'xwayland_wrapper|benchmark_gles' || true
