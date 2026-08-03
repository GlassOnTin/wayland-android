#!/bin/bash
# Build virglrenderer's two Android server executables that ship in
# core/wayland's jniLibs: libvirgl_test_server.so and libvirgl_render_server.so.
#
# Why this exists (#493): these were the last two binaries in
# core/wayland/src/main/jniLibs with no build script anywhere — they had been
# compiled by hand and committed. F-Droid `scandelete`s that directory, so its
# builds shipped without them and GPU acceleration in the Linux container was
# simply absent there, silently, while our own builds shipped the committed
# copies. build-native-helpers.sh closed the same gap for the other two.
#
# Both are ELF *executables* renamed lib*.so. Android's installer extracts
# anything matching lib*.so into nativeLibraryDir, the only place an app may
# exec from under W^X. They are NOT shared libraries; do not dlopen them.
#
# Prerequisites: sysroot/$ABI, built by build-android.sh (or
# build_liblabwc_android.sh, which calls it). This needs libdrm, EGL and GLESv2
# pkg-config files from there. libepoxy is a submodule that nothing else builds,
# so it is built here into the same sysroot.
#
# Usage:
#   ./build-virgl-android.sh                 # arm64-v8a
#   ABI=x86_64 ./build-virgl-android.sh      # another ABI

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

ABI="${ABI:-arm64-v8a}"
# 28, not core/wayland's minSdk 26: the sysroot these link against is built at
# 28 by build-android.sh, and mixing API levels across a static link is how you
# get a missing symbol at runtime rather than at build time.
API="${API:-28}"

case "$ABI" in
    arm64-v8a)   TRIPLE=aarch64-linux-android;    CPU_FAMILY=aarch64; CPU=aarch64 ;;
    x86_64)      TRIPLE=x86_64-linux-android;     CPU_FAMILY=x86_64;  CPU=x86_64 ;;
    armeabi-v7a) TRIPLE=armv7a-linux-androideabi; CPU_FAMILY=arm;     CPU=armv7 ;;
    *) echo "unsupported ABI: $ABI" >&2; exit 1 ;;
esac
# The clang wrapper is named after the *toolchain* triple, which for armv7 is
# not the compiler prefix — arm-linux-androideabi for ar/ranlib, armv7a- for cc.
BINUTILS_TRIPLE="$TRIPLE"
[ "$ABI" = "armeabi-v7a" ] && BINUTILS_TRIPLE=arm-linux-androideabi

# NDK discovery mirrors build-native-helpers.sh: explicit env wins, else newest
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
CXX="$TC/${TRIPLE}${API}-clang++"
[ -x "$CC" ] || { echo "no NDK clang at $CC" >&2; exit 1; }

PREFIX="$SCRIPT_DIR/sysroot/$ABI"
BUILDDIR="$SCRIPT_DIR/build/$ABI"
OUT_DIR="$SCRIPT_DIR/jniLibs/$ABI"

# Fail early and by name. Without this the first failure is a meson dependency
# error thirty lines into a log, which reads as "virgl is broken" rather than
# "you skipped a prerequisite".
for pc in libdrm egl glesv2; do
    [ -f "$PREFIX/lib/pkgconfig/$pc.pc" ] || {
        echo "ERROR: $PREFIX/lib/pkgconfig/$pc.pc missing — run build-android.sh for $ABI first" >&2
        exit 1
    }
done
[ -f "$SCRIPT_DIR/libepoxy/meson.build" ] || {
    echo "ERROR: libepoxy submodule not checked out — git submodule update --init libepoxy" >&2
    exit 1
}
[ -f "$SCRIPT_DIR/virglrenderer/meson.build" ] || {
    echo "ERROR: virglrenderer submodule not checked out — git submodule update --init virglrenderer" >&2
    exit 1
}

mkdir -p "$OUT_DIR" "$BUILDDIR"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig"
export PKG_CONFIG_LIBDIR="$PKG_CONFIG_PATH"

echo "=== virglrenderer for Android: ABI=$ABI API=$API NDK=$NDK ==="

# Self-contained cross file rather than reusing build-android.sh's: that one
# lives under build/$ABI and disappears with a clean, which would make this
# script work or fail depending on whether someone had just built labwc.
CROSSFILE="$BUILDDIR/cross-virgl.txt"
cat > "$CROSSFILE" <<CROSS
[binaries]
c = '$CC'
cpp = '$CXX'
ar = '$TC/llvm-ar'
strip = '$TC/llvm-strip'
ranlib = '$TC/llvm-ranlib'
pkg-config = '/usr/bin/pkg-config'

[built-in options]
c_args = ['-I$PREFIX/include', '-fPIC']
cpp_args = ['-I$PREFIX/include', '-fPIC']

[properties]
pkg_config_libdir = '$PKG_CONFIG_PATH'
needs_exe_wrapper = true

[host_machine]
system = 'android'
cpu_family = '$CPU_FAMILY'
cpu = '$CPU'
endian = 'little'
CROSS

# ---- libepoxy ----
# virglrenderer requires epoxy unconditionally and additionally checks that its
# pkg-config declares epoxy_has_egl=1, so -Degl=yes is load-bearing, not tidiness.
# Nothing else in this tree builds epoxy — build-android.sh doesn't know about it.
build_epoxy() {
    echo "--- libepoxy ---"
    rm -rf "$BUILDDIR/epoxy"
    meson setup "$BUILDDIR/epoxy" "$SCRIPT_DIR/libepoxy" \
        --cross-file "$CROSSFILE" \
        --prefix="$PREFIX" --libdir=lib \
        --default-library=static --buildtype=release \
        -Degl=yes -Dglx=no -Dx11=false -Dtests=false -Ddocs=false
    ninja -C "$BUILDDIR/epoxy"
    ninja -C "$BUILDDIR/epoxy" install
}

# ---- virglrenderer ----
# -Dvenus=true is what produces virgl_render_server at all: meson.build sets
#   with_render_server = with_venus
# so without venus the server/ subdir is never entered and only the vtest
# server appears. venus is also the renderer that actually works in the proot
# guest, so this is not a spare part.
#
# -Dplatforms=egl with no GBM: the fork takes an Android branch that uses
# AHardwareBuffer instead, defines VIRGL_ANDROID_AS_LINUX so Mesa's util code
# avoids AOSP-internal headers, and force-links EGL/GLESv2.
#
# render-server-worker stays at its default 'process' — the 'minijail' mode
# needs libminijail, which the NDK does not provide.
build_virgl() {
    echo "--- virglrenderer ---"
    rm -rf "$BUILDDIR/virgl"
    meson setup "$BUILDDIR/virgl" "$SCRIPT_DIR/virglrenderer" \
        --cross-file "$CROSSFILE" \
        --prefix="$PREFIX" --libdir=lib \
        --default-library=static --buildtype=release \
        -Dplatforms=egl -Dvenus=true -Dtests=false -Dfuzzer=false
    ninja -C "$BUILDDIR/virgl"
}

install_binaries() {
    echo "--- installing into $OUT_DIR ---"
    local ok=1
    for pair in "vtest/virgl_test_server:libvirgl_test_server.so" \
                "server/virgl_render_server:libvirgl_render_server.so"; do
        local src="$BUILDDIR/virgl/${pair%%:*}"
        local dst="$OUT_DIR/${pair##*:}"
        if [ ! -f "$src" ]; then
            echo "ERROR: expected $src — meson did not produce it" >&2
            ok=0
            continue
        fi
        cp "$src" "$dst"
        "$TC/llvm-strip" "$dst"
        # An executable that is not PIE is refused outright by Android's linker,
        # and a shared object here would mean meson built the wrong target. Check
        # what we produced rather than trusting the flags that produced it.
        case "$(od -An -tx1 -j16 -N2 "$dst" | tr -d ' ')" in
            0300) : ;;  # ET_DYN — PIE executable or .so
            *) echo "ERROR: $dst is not ET_DYN — Android will refuse to exec it" >&2; ok=0 ;;
        esac
    done
    [ "$ok" = 1 ] || exit 1
}

build_epoxy
build_virgl
install_binaries

echo "=== built into $OUT_DIR ==="
ls -la "$OUT_DIR" | grep -E 'virgl_(test|render)_server' || true
