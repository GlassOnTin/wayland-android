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

# Step 1: Build all dependencies (wayland, wlroots, cairo, pango, etc.)
bash "$SCRIPT_DIR/build-android.sh"

# Step 2: Generate Android stubs for libinput/DRM symbols unused on Android
# Auto-detect NDK: ANDROID_NDK_HOME > newest under ANDROID_HOME/ndk/ > newest under ANDROID_SDK_ROOT/ndk/
if [ -z "${ANDROID_NDK_HOME:-}" ]; then
    SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}"
    if [ -n "$SDK" ] && [ -d "$SDK/ndk" ]; then
        ANDROID_NDK_HOME="$(ls -d "$SDK/ndk"/*/ 2>/dev/null | sort -V | tail -1)"
        ANDROID_NDK_HOME="${ANDROID_NDK_HOME%/}"
    fi
fi
NDK="${ANDROID_NDK_HOME:?ANDROID_NDK_HOME must be set or an NDK must exist under ANDROID_HOME/ndk/}"
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

# Generate and compile stubs
bash "$SCRIPT_DIR/gen-stubs.sh" > /tmp/android_stubs.c
# Add xcb_ewmh stubs (m4 generation may produce empty library)
cat >> /tmp/android_stubs.c << 'EWMH'
void *xcb_ewmh_get_wm_icon_from_reply() { return 0; }
void *xcb_ewmh_get_wm_icon_iterator() { return 0; }
void *xcb_ewmh_get_wm_icon_next() { return 0; }
void *xcb_ewmh_get_wm_strut_partial_from_reply() { return 0; }
EWMH
$CC -c /tmp/android_stubs.c -o /tmp/android_stubs.o -fPIC
$AR rcs "$PREFIX/lib/libandroid_stubs.a" /tmp/android_stubs.o

# Step 3: Build labwc + wlroots (with GLES2) + link final .so
# Remove renderer-less wlroots from step 1 so build-labwc.sh rebuilds with GLES2
rm -f "$PREFIX/lib/libwlroots-0.19.a"
rm -rf "$SCRIPT_DIR/build/$ABI/wlroots"
bash "$SCRIPT_DIR/build-labwc.sh" "$ABI"

# Step 4: Output location
echo ""
echo "=== Output: jniLibs/$ABI/liblabwc_android.so ==="
ls -lh "$SCRIPT_DIR/jniLibs/$ABI/liblabwc_android.so"
