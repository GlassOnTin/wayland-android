#!/bin/bash
set -euo pipefail
# Build labwc + JNI bridge into liblabwc_android.so
# Run AFTER build-android.sh has built all dependencies.
#
# Usage: ./build-labwc.sh [ABI]
#   ABI: arm64-v8a (default) or x86_64

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ABI="${1:-arm64-v8a}"

# Resolve NDK (inherited from build_liblabwc_android.sh or auto-detect)
if [ -z "${ANDROID_NDK_HOME:-}" ]; then
    SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}"
    if [ -n "$SDK" ] && [ -d "$SDK/ndk" ]; then
        export ANDROID_NDK_HOME="$(ls -d "$SDK/ndk"/*/ 2>/dev/null | sort -V | tail -1)"
        ANDROID_NDK_HOME="${ANDROID_NDK_HOME%/}"
    fi
fi
NDK="${ANDROID_NDK_HOME:?ANDROID_NDK_HOME must be set}"
TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64"

if [ "$ABI" = "arm64-v8a" ]; then
    TARGET="aarch64-linux-android28"
elif [ "$ABI" = "x86_64" ]; then
    TARGET="x86_64-linux-android28"
else
    echo "Unsupported ABI: $ABI" >&2; exit 1
fi

CC="$TOOLCHAIN/bin/${TARGET}-clang"
CXX="$TOOLCHAIN/bin/${TARGET}-clang++"
STRIP="$TOOLCHAIN/bin/llvm-strip"
SYSROOT="$SCRIPT_DIR/sysroot/$ABI"
BUILDDIR="$SCRIPT_DIR/build/$ABI"
NDK_SYSROOT="$TOOLCHAIN/sysroot"

echo "=== Building labwc for $ABI ==="

# Build labwc if not already built
# Install wlroots backend/type headers that labwc includes but we don't build.
# (We use -Dbackends=[] but labwc #includes the headers for type defs.)
WLR_INC="$SYSROOT/include/wlroots-0.19/wlr"
mkdir -p "$WLR_INC/backend" "$WLR_INC/types"
for hdr in drm.h wayland.h headless.h multi.h libinput.h session.h x11.h; do
    src="$SCRIPT_DIR/wlroots/include/wlr/backend/$hdr"
    [ -f "$src" ] && cp "$src" "$WLR_INC/backend/"
done
# DRM lease header used by labwc server.c
for hdr in wlr_drm_lease_v1.h; do
    src="$SCRIPT_DIR/wlroots/include/wlr/types/$hdr"
    [ -f "$src" ] && cp "$src" "$WLR_INC/types/"
done
# Also copy to legacy path for build-labwc.sh JNI includes
mkdir -p "$SYSROOT/include/wlr/backend"
for hdr in drm.h wayland.h headless.h multi.h libinput.h session.h x11.h; do
    src="$SCRIPT_DIR/wlroots/include/wlr/backend/$hdr"
    [ -f "$src" ] && cp "$src" "$SYSROOT/include/wlr/backend/"
done

if [ ! -f "$BUILDDIR/labwc/liblabwc.a" ]; then
    echo "--- labwc (Meson) ---"
    CROSSFILE="$BUILDDIR/cross.txt"
    rm -rf "$BUILDDIR/labwc"
    meson setup "$BUILDDIR/labwc" "$SCRIPT_DIR/labwc" \
        --cross-file "$CROSSFILE" \
        --prefix="$SYSROOT" \
        --default-library=static \
        -Dxwayland=enabled -Dsvg=disabled -Dicon=disabled \
        -Dnls=disabled -Dman-pages=disabled \
        -Db_pie=false
    ninja -C "$BUILDDIR/labwc" -j"$(nproc)"
fi

# Create synthetic pkg-config files for EGL/GLES2
mkdir -p "$SYSROOT/lib/pkgconfig"
cat > "$SYSROOT/lib/pkgconfig/egl.pc" << EOF
prefix=$NDK_SYSROOT/usr
libdir=\${prefix}/lib/$TARGET
includedir=\${prefix}/include
Name: egl
Description: Android EGL
Version: 1.5
Libs: -L\${libdir} -lEGL
Cflags: -I\${includedir}
EOF
cat > "$SYSROOT/lib/pkgconfig/glesv2.pc" << EOF
prefix=$NDK_SYSROOT/usr
libdir=\${prefix}/lib/$TARGET
includedir=\${prefix}/include
Name: glesv2
Description: Android OpenGL ES 2.0
Version: 2.0
Libs: -L\${libdir} -lGLESv2
Cflags: -I\${includedir}
EOF

# Stub gbm.h (Android has no GBM)
mkdir -p "$SYSROOT/include"
cp "$SCRIPT_DIR/sysroot/arm64-v8a/include/gbm.h" "$SYSROOT/include/gbm.h" 2>/dev/null || true

# Build wlroots with GLES2 if not already built
if [ ! -f "$SYSROOT/lib/libwlroots-0.19.a" ]; then
    echo "--- wlroots (with GLES2) ---"
    CROSSFILE="$BUILDDIR/cross.txt"
    rm -rf "$BUILDDIR/wlroots"
    meson setup "$BUILDDIR/wlroots" "$SCRIPT_DIR/wlroots" \
        --cross-file "$CROSSFILE" \
        --prefix="$SYSROOT" \
        --default-library=static \
        -Dbackends=[] -Drenderers=gles2 -Dallocators=[] \
        -Dexamples=false -Dxwayland=enabled -Dsession=disabled \
        -Dcolor-management=disabled -Dlibliftoff=disabled \
        -Dxcb-errors=disabled -Dwerror=false
    ninja -C "$BUILDDIR/wlroots" -j"$(nproc)"
    ninja -C "$BUILDDIR/wlroots" install
fi

echo "--- Compiling JNI bridge + allocator + presenter ---"

INCLUDES="-I$SYSROOT/include -I$SYSROOT/include/wlroots-0.19 -I$SYSROOT/include/pixman-1 \
  -I$SCRIPT_DIR/labwc/include -I$BUILDDIR/labwc/include -I$BUILDDIR/labwc/protocols \
  -I$SCRIPT_DIR/wlroots/include -I$BUILDDIR/wlroots/include \
  -I$SYSROOT/include/glib-2.0 -I$SYSROOT/lib/glib-2.0/include \
  -I$SYSROOT/include/cairo -I$SYSROOT/include/pango-1.0 \
  -I$SYSROOT/include/harfbuzz -I$SYSROOT/include/freetype2 \
  -I$SYSROOT/include/libxml2 -I$SYSROOT/include/libdrm \
  -I$NDK_SYSROOT/usr/include -I$SCRIPT_DIR"

FLAGS="-fPIC -DWLR_USE_UNSTABLE -D_GNU_SOURCE"

$CC -c "$SCRIPT_DIR/jni_bridge.c" -o "$BUILDDIR/jni_bridge.o" $FLAGS $INCLUDES
$CC -c "$SCRIPT_DIR/ahb_allocator.c" -o "$BUILDDIR/ahb_allocator.o" $FLAGS $INCLUDES
$CC -c "$SCRIPT_DIR/buffer_format_utils.c" -o "$BUILDDIR/buffer_format_utils.o" $FLAGS $INCLUDES

# buffer_presenter needs weak symbols for API 29+ functions
$CC -c "$SCRIPT_DIR/buffer_presenter.c" -o "$BUILDDIR/buffer_presenter.o" \
  -fPIC -D_GNU_SOURCE -I$NDK_SYSROOT/usr/include -I$SCRIPT_DIR

echo "--- Linking liblabwc_android.so ---"

mkdir -p "$SCRIPT_DIR/jniLibs/$ABI"

$CC -shared -o "$SCRIPT_DIR/jniLibs/$ABI/liblabwc_android.so" \
  "$BUILDDIR/jni_bridge.o" "$BUILDDIR/ahb_allocator.o" \
  "$BUILDDIR/buffer_format_utils.o" "$BUILDDIR/buffer_presenter.o" \
  -Wl,--whole-archive "$BUILDDIR/labwc/liblabwc.a" -Wl,--no-whole-archive \
  "$SYSROOT/lib/libwlroots-0.19.a" "$SYSROOT/lib/libwayland-server.a" \
  "$SYSROOT/lib/libxkbcommon.a" "$SYSROOT/lib/libpixman-1.a" "$SYSROOT/lib/libdrm.a" \
  "$SYSROOT/lib/libcairo.a" "$SYSROOT/lib/libpangocairo-1.0.a" "$SYSROOT/lib/libpango-1.0.a" \
  "$SYSROOT/lib/libpangoft2-1.0.a" "$SYSROOT/lib/libharfbuzz.a" "$SYSROOT/lib/libfreetype.a" \
  "$SYSROOT/lib/libfontconfig.a" "$SYSROOT/lib/libfribidi.a" "$SYSROOT/lib/libpng16.a" \
  "$SYSROOT/lib/libexpat.a" "$SYSROOT/lib/libxml2.a" "$SYSROOT/lib/libglib-2.0.a" \
  "$SYSROOT/lib/libgobject-2.0.a" "$SYSROOT/lib/libgio-2.0.a" "$SYSROOT/lib/libgmodule-2.0.a" \
  "$SYSROOT/lib/libgthread-2.0.a" "$SYSROOT/lib/libffi.a" "$SYSROOT/lib/libintl.a" \
  "$SYSROOT/lib/libpcre2-8.a" "$SYSROOT/lib/libwayland-client.a" \
  "$SYSROOT/lib/libxcb.a" "$SYSROOT/lib/libxcb-composite.a" \
  "$SYSROOT/lib/libxcb-ewmh.a" "$SYSROOT/lib/libxcb-icccm.a" \
  "$SYSROOT/lib/libxcb-render.a" "$SYSROOT/lib/libxcb-res.a" \
  "$SYSROOT/lib/libxcb-xfixes.a" "$SYSROOT/lib/libxcb-shm.a" \
  "$SYSROOT/lib/libXau.a" "$SYSROOT/lib/libXdmcp.a" \
  "$SYSROOT/lib/libandroid_stubs.a" \
  -llog -landroid -lm -lz -lEGL -lGLESv2 -lnativewindow

$STRIP "$SCRIPT_DIR/jniLibs/$ABI/liblabwc_android.so"

SIZE=$(stat -c%s "$SCRIPT_DIR/jniLibs/$ABI/liblabwc_android.so")
echo "=== Built: jniLibs/$ABI/liblabwc_android.so ($((SIZE / 1024 / 1024)) MB) ==="
