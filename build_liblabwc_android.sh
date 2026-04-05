#!/bin/bash
set -euo pipefail

# Build liblabwc_android.so from source for Android.
# Single entry point for F-Droid and local builds.
#
# Usage: ABI=arm64-v8a ./build_liblabwc_android.sh
#        ABI=x86_64  ./build_liblabwc_android.sh
#
# Prerequisites (F-Droid sudo block):
#   apt-get install -y meson ninja-build pkg-config libexpat1-dev libffi-dev \
#       automake autoconf libtool libtool-bin libwayland-dev python3 bison g++
#
# Environment:
#   ANDROID_NDK_HOME — path to NDK r28+
#   ABI — arm64-v8a (default) or x86_64

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
export ABI="${ABI:-arm64-v8a}"

echo "=== Building liblabwc_android.so for $ABI ==="

# Auto-detect NDK: ANDROID_NDK_HOME > newest under ANDROID_HOME/ndk/ > newest under ANDROID_SDK_ROOT/ndk/
if [ -z "${ANDROID_NDK_HOME:-}" ]; then
    SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}"
    if [ -n "$SDK" ] && [ -d "$SDK/ndk" ]; then
        ANDROID_NDK_HOME="$(ls -d "$SDK/ndk"/*/ 2>/dev/null | sort -V | tail -1)"
        ANDROID_NDK_HOME="${ANDROID_NDK_HOME%/}"
    fi
fi
NDK="${ANDROID_NDK_HOME:?ANDROID_NDK_HOME must be set or an NDK must exist under ANDROID_HOME/ndk/}"
export ANDROID_NDK_HOME="$NDK"
TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64"
PREFIX="$SCRIPT_DIR/sysroot/$ABI"

if [ "$ABI" = "arm64-v8a" ]; then
    TARGET="aarch64-linux-android28"
elif [ "$ABI" = "x86_64" ]; then
    TARGET="x86_64-linux-android28"
else
    echo "Unsupported ABI: $ABI" >&2; exit 1
fi

CC="$TOOLCHAIN/bin/${TARGET}-clang"
AR="$TOOLCHAIN/bin/llvm-ar"

# Step 1: Build all dependencies (wayland, wlroots, cairo, pango, etc.)
bash "$SCRIPT_DIR/build-android.sh"

# Copy libinput.h from submodule (labwc includes it but we don't build libinput)
cp "$SCRIPT_DIR/libinput/src/libinput.h" "$PREFIX/include/" 2>/dev/null || true

# Stub libudev.h (Android has no udev)
if [ ! -f "$PREFIX/include/libudev.h" ]; then
    cat > "$PREFIX/include/libudev.h" << 'UDEV'
#ifndef LIBUDEV_H
#define LIBUDEV_H
struct udev;
struct udev_device;
#endif
UDEV
fi

# Step 2: Build labwc + wlroots (with GLES2 renderer)
# Remove renderer-less wlroots from step 1 so build-labwc.sh rebuilds with GLES2
rm -f "$PREFIX/lib/libwlroots-0.19.a"
rm -rf "$SCRIPT_DIR/build/$ABI/wlroots"

# Create a minimal stubs library so labwc can link for the first pass
echo '/* minimal stubs */' > /tmp/android_stubs.c
echo 'void *libinput_udev_create_context() { return 0; }' >> /tmp/android_stubs.c
$CC -c /tmp/android_stubs.c -o /tmp/android_stubs.o -fPIC
$AR rcs "$PREFIX/lib/libandroid_stubs.a" /tmp/android_stubs.o

bash "$SCRIPT_DIR/build-labwc.sh" "$ABI"

# Step 3: Generate stubs from built labwc (now that liblabwc.a exists)
# Then rebuild the stubs library and relink
echo "=== Generating stubs from built labwc ==="
bash "$SCRIPT_DIR/gen-stubs.sh" > /tmp/android_stubs.c
# Add xcb_ewmh stubs only if not already defined in sysroot
# (clean builds with working m4 produce a real libxcb-ewmh.a)
NM="$TOOLCHAIN/bin/llvm-nm"
SYSROOT_DEFINED=$( $NM --defined-only "$PREFIX/lib/"*.a 2>/dev/null | awk '{print $3}' | sort -u )
for sym in xcb_ewmh_get_wm_icon_from_reply xcb_ewmh_get_wm_icon_iterator \
           xcb_ewmh_get_wm_icon_next xcb_ewmh_get_wm_strut_partial_from_reply; do
    if ! echo "$SYSROOT_DEFINED" | grep -qx "$sym"; then
        echo "void *${sym}() { return 0; }" >> /tmp/android_stubs.c
    fi
done
$CC -c /tmp/android_stubs.c -o /tmp/android_stubs.o -fPIC
$AR rcs "$PREFIX/lib/libandroid_stubs.a" /tmp/android_stubs.o

# Step 4: Relink final .so with correct stubs
bash "$SCRIPT_DIR/build-labwc.sh" "$ABI"

# Step 5: Output location
echo ""
echo "=== Output: jniLibs/$ABI/liblabwc_android.so ==="
ls -lh "$SCRIPT_DIR/jniLibs/$ABI/liblabwc_android.so"
