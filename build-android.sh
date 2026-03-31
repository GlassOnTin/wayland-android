#!/bin/bash
set -euo pipefail

# Cross-compile wayland + wlroots dependency chain for Android arm64
# Designed to run on F-Droid buildserver (Debian) or local Linux host
#
# Prerequisites (F-Droid sudo block):
#   apt-get install -y meson ninja-build pkg-config libexpat1-dev libffi-dev
#
# Environment:
#   ANDROID_NDK_HOME — path to NDK r28+
#   ABI — arm64-v8a or x86_64 (default: arm64-v8a)

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ABI="${ABI:-arm64-v8a}"

# Resolve NDK
NDK="${ANDROID_NDK_HOME:-${ANDROID_SDK_ROOT:-/home/ian/Android/Sdk}/ndk/28.2.13676358}"
TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64"

if [ "$ABI" = "arm64-v8a" ]; then
    ARCH_TRIPLE="aarch64-linux-android"
    MESON_CPU_FAMILY="aarch64"
    MESON_CPU="aarch64"
elif [ "$ABI" = "x86_64" ]; then
    ARCH_TRIPLE="x86_64-linux-android"
    MESON_CPU_FAMILY="x86_64"
    MESON_CPU="x86_64"
else
    echo "Unsupported ABI: $ABI" >&2; exit 1
fi

API_LEVEL=28
CC="$TOOLCHAIN/bin/${ARCH_TRIPLE}${API_LEVEL}-clang"
CXX="$TOOLCHAIN/bin/${ARCH_TRIPLE}${API_LEVEL}-clang++"
AR="$TOOLCHAIN/bin/llvm-ar"
RANLIB="$TOOLCHAIN/bin/llvm-ranlib"
STRIP="$TOOLCHAIN/bin/llvm-strip"

PREFIX="$SCRIPT_DIR/sysroot/$ABI"
BUILDDIR="$SCRIPT_DIR/build/$ABI"
mkdir -p "$PREFIX"/{lib/pkgconfig,share/pkgconfig,include} "$BUILDDIR"

export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig"
export PKG_CONFIG_LIBDIR="$PKG_CONFIG_PATH"

# Generate Meson cross-file
CROSSFILE="$BUILDDIR/cross.txt"
cat > "$CROSSFILE" <<CROSS
[binaries]
c = '$CC'
cpp = '$CXX'
ar = '$AR'
strip = '$STRIP'
ranlib = '$RANLIB'
pkg-config = '/usr/bin/pkg-config'
wayland-scanner = '/usr/bin/wayland-scanner'

[built-in options]
c_args = ['-I$PREFIX/include', '-fPIC']
cpp_args = ['-I$PREFIX/include', '-fPIC']

[properties]
pkg_config_libdir = '$PKG_CONFIG_PATH'
needs_exe_wrapper = true

[host_machine]
system = 'android'
cpu_family = '$MESON_CPU_FAMILY'
cpu = '$MESON_CPU'
endian = 'little'
CROSS

echo "=== Building for $ABI with NDK at $NDK ==="

# ---- libffi (autotools) ----
build_libffi() {
    echo "--- libffi ---"
    cd "$SCRIPT_DIR/libffi"
    [ -f configure ] || autoreconf -fi
    rm -rf "$BUILDDIR/libffi"
    mkdir -p "$BUILDDIR/libffi"
    cd "$BUILDDIR/libffi"
    CC="$CC" AR="$AR" RANLIB="$RANLIB" STRIP="$STRIP" CFLAGS="-fPIC" \
        "$SCRIPT_DIR/libffi/configure" \
        --host="$ARCH_TRIPLE" \
        --prefix="$PREFIX" \
        --disable-shared --enable-static --disable-docs
    make -j"$(nproc)"
    make install
}

# ---- libexpat (CMake) ----
build_expat() {
    echo "--- expat ---"
    rm -rf "$BUILDDIR/expat"
    cmake -B "$BUILDDIR/expat" -S "$SCRIPT_DIR/libexpat/expat" \
        -DCMAKE_SYSTEM_NAME=Android \
        -DCMAKE_ANDROID_NDK="$NDK" \
        -DCMAKE_ANDROID_ARCH_ABI="$ABI" \
        -DCMAKE_ANDROID_API="$API_LEVEL" \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DEXPAT_BUILD_EXAMPLES=OFF -DEXPAT_BUILD_TESTS=OFF \
        -DEXPAT_BUILD_TOOLS=OFF -DEXPAT_SHARED_LIBS=OFF
    cmake --build "$BUILDDIR/expat" -j"$(nproc)"
    cmake --install "$BUILDDIR/expat"
}

# ---- wayland (Meson) ----
build_wayland() {
    echo "--- wayland ---"
    rm -rf "$BUILDDIR/wayland"
    # Temporarily restore system PKG_CONFIG so meson finds host wayland-scanner
    PKG_CONFIG_PATH="/usr/lib/x86_64-linux-gnu/pkgconfig:/usr/share/pkgconfig:$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig" \
    PKG_CONFIG_LIBDIR="/usr/lib/x86_64-linux-gnu/pkgconfig:/usr/share/pkgconfig:$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig" \
    meson setup "$BUILDDIR/wayland" "$SCRIPT_DIR/wayland" \
        --cross-file "$CROSSFILE" \
        --prefix="$PREFIX" \
        --default-library=static \
        -Ddocumentation=false -Dtests=false -Ddtd_validation=false -Dscanner=false
    ninja -C "$BUILDDIR/wayland" -j"$(nproc)"
    ninja -C "$BUILDDIR/wayland" install
    # Create a wayland-scanner.pc pointing to the host binary (since we skipped building the scanner)
    cat > "$PREFIX/lib/pkgconfig/wayland-scanner.pc" <<SCANNERPC
prefix=/usr
wayland_scanner=/usr/bin/wayland-scanner
Name: Wayland Scanner
Description: Wayland protocol scanner
Version: 1.24.0
SCANNERPC
}

# ---- wayland-protocols (Meson, header-only install) ----
build_wayland_protocols() {
    echo "--- wayland-protocols ---"
    rm -rf "$BUILDDIR/wayland-protocols"
    # Override wayland_scanner to use the HOST binary, not the cross-compiled one
    PKG_CONFIG_PATH="/usr/lib/x86_64-linux-gnu/pkgconfig:/usr/share/pkgconfig:$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig" \
    PKG_CONFIG_LIBDIR="/usr/lib/x86_64-linux-gnu/pkgconfig:/usr/share/pkgconfig:$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig" \
    meson setup "$BUILDDIR/wayland-protocols" "$SCRIPT_DIR/wayland-protocols" \
        --cross-file "$CROSSFILE" \
        --prefix="$PREFIX" \
        -Dtests=false
    ninja -C "$BUILDDIR/wayland-protocols" -j"$(nproc)"
    ninja -C "$BUILDDIR/wayland-protocols" install
}

# ---- pixman (Meson) ----
build_pixman() {
    echo "--- pixman ---"
    rm -rf "$BUILDDIR/pixman"
    meson setup "$BUILDDIR/pixman" "$SCRIPT_DIR/pixman" \
        --cross-file "$CROSSFILE" \
        --prefix="$PREFIX" \
        --default-library=static \
        -Dtests=disabled -Ddemos=disabled -Dgtk=disabled \
        -Dlibpng=disabled -Dopenmp=disabled
    ninja -C "$BUILDDIR/pixman" -j"$(nproc)"
    ninja -C "$BUILDDIR/pixman" install
}

# ---- xkbcommon (Meson) ----
build_xkbcommon() {
    echo "--- xkbcommon ---"
    rm -rf "$BUILDDIR/xkbcommon"
    meson setup "$BUILDDIR/xkbcommon" "$SCRIPT_DIR/libxkbcommon" \
        --cross-file "$CROSSFILE" \
        --prefix="$PREFIX" \
        --default-library=static \
        -Denable-docs=false -Denable-tools=false \
        -Denable-x11=false -Denable-wayland=false \
        -Denable-xkbregistry=false
    ninja -C "$BUILDDIR/xkbcommon" -j"$(nproc)"
    ninja -C "$BUILDDIR/xkbcommon" install
}

# ---- libdrm (Meson, headers only for wlroots) ----
build_libdrm() {
    echo "--- libdrm ---"
    rm -rf "$BUILDDIR/libdrm"
    meson setup "$BUILDDIR/libdrm" "$SCRIPT_DIR/libdrm" \
        --cross-file "$CROSSFILE" \
        --prefix="$PREFIX" \
        --default-library=static \
        -Dintel=disabled -Dradeon=disabled -Damdgpu=disabled \
        -Dnouveau=disabled -Dvmwgfx=disabled -Dfreedreno=disabled \
        -Dvc4=disabled -Detnaviv=disabled -Dtests=false \
        -Dman-pages=disabled -Dvalgrind=disabled -Dcairo-tests=disabled
    ninja -C "$BUILDDIR/libdrm" -j"$(nproc)"
    ninja -C "$BUILDDIR/libdrm" install
}

# Helper: CMake cross-compile for Android
cmake_android() {
    local name="$1"; shift
    local srcdir="$1"; shift
    echo "--- $name (CMake) ---"
    rm -rf "$BUILDDIR/$name"
    cmake -B "$BUILDDIR/$name" -S "$srcdir" \
        -DCMAKE_SYSTEM_NAME=Android \
        -DCMAKE_ANDROID_NDK="$NDK" \
        -DCMAKE_ANDROID_ARCH_ABI="$ABI" \
        -DCMAKE_ANDROID_API="$API_LEVEL" \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DCMAKE_FIND_ROOT_PATH="$PREFIX" \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DBUILD_SHARED_LIBS=OFF \
        "$@"
    cmake --build "$BUILDDIR/$name" -j"$(nproc)"
    cmake --install "$BUILDDIR/$name"
}

# Helper: Meson cross-compile
meson_android() {
    local name="$1"; shift
    local srcdir="$1"; shift
    echo "--- $name (Meson) ---"
    rm -rf "$BUILDDIR/$name"
    meson setup "$BUILDDIR/$name" "$srcdir" \
        --cross-file "$CROSSFILE" \
        --prefix="$PREFIX" \
        --default-library=static \
        "$@"
    ninja -C "$BUILDDIR/$name" -j"$(nproc)"
    ninja -C "$BUILDDIR/$name" install
}

# ---- libxml2 (CMake) ----
build_libxml2() {
    cmake_android libxml2 "$SCRIPT_DIR/libxml2" \
        -DLIBXML2_WITH_PYTHON=OFF -DLIBXML2_WITH_LZMA=OFF \
        -DLIBXML2_WITH_ZLIB=OFF -DLIBXML2_WITH_TESTS=OFF \
        -DLIBXML2_WITH_PROGRAMS=OFF -DLIBXML2_WITH_ICU=OFF
}

# ---- libpng (CMake) ----
build_libpng() {
    cmake_android libpng "$SCRIPT_DIR/libpng" \
        -DPNG_SHARED=OFF -DPNG_TESTS=OFF -DPNG_TOOLS=OFF
}

# ---- freetype (CMake, first pass without harfbuzz) ----
build_freetype() {
    cmake_android freetype "$SCRIPT_DIR/freetype" \
        -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BZIP2=ON \
        -DFT_DISABLE_BROTLI=ON -DFT_DISABLE_PNG=OFF
}

# ---- fontconfig (Meson) ----
build_fontconfig() {
    meson_android fontconfig "$SCRIPT_DIR/fontconfig" \
        -Ddoc=disabled -Dtests=disabled -Dtools=disabled -Dcache-build=disabled
}

# ---- pcre2 (CMake) ----
build_pcre2() {
    cmake_android pcre2 "$SCRIPT_DIR/pcre2" \
        -DPCRE2_BUILD_PCRE2GREP=OFF -DPCRE2_BUILD_TESTS=OFF
}

# ---- glib (Meson) ----
build_glib() {
    meson_android glib "$SCRIPT_DIR/glib" \
        -Dtests=false -Dglib_debug=disabled -Dlibelf=disabled \
        -Dselinux=disabled -Dxattr=false -Dlibmount=disabled -Dnls=disabled
}

# ---- fribidi (Meson) ----
build_fribidi() {
    meson_android fribidi "$SCRIPT_DIR/fribidi" \
        -Dtests=false -Ddocs=false
}

# ---- harfbuzz (Meson, C++) ----
build_harfbuzz() {
    meson_android harfbuzz "$SCRIPT_DIR/harfbuzz" \
        -Dtests=disabled -Ddocs=disabled -Dcairo=disabled \
        -Dglib=enabled -Dfreetype=enabled
}

# ---- freetype pass 2 (with harfbuzz) ----
build_freetype_pass2() {
    cmake_android freetype "$SCRIPT_DIR/freetype" \
        -DFT_DISABLE_HARFBUZZ=OFF -DFT_DISABLE_BZIP2=ON \
        -DFT_DISABLE_BROTLI=ON -DFT_DISABLE_PNG=OFF
}

# ---- cairo (Meson) ----
build_cairo() {
    meson_android cairo "$SCRIPT_DIR/cairo" \
        -Dtests=disabled -Dspectre=disabled -Dtee=disabled \
        -Dxcb=disabled -Dxlib=disabled -Dquartz=disabled \
        -Dpng=enabled -Dfreetype=enabled -Dfontconfig=enabled -Dglib=enabled
}

# ---- pango (Meson) ----
build_pango() {
    meson_android pango "$SCRIPT_DIR/pango" \
        -Dintrospection=disabled -Dgtk_doc=false
}

# Execute all builds
build_libffi
build_expat
build_wayland
build_wayland_protocols
build_pixman
build_xkbcommon
build_libdrm

# wlroots (uses GlassOnTin/wlroots fork with Android patches)
echo "--- wlroots ---"
rm -rf "$BUILDDIR/wlroots"
meson setup "$BUILDDIR/wlroots" "$SCRIPT_DIR/wlroots" \
    --cross-file "$CROSSFILE" \
    --prefix="$PREFIX" \
    --default-library=static \
    -Dbackends=[] -Drenderers=[] -Dallocators=[] \
    -Dexamples=false -Dxwayland=disabled -Dsession=disabled \
    -Dcolor-management=disabled -Dlibliftoff=disabled \
    -Dxcb-errors=disabled -Dwerror=false
ninja -C "$BUILDDIR/wlroots" -j"$(nproc)"
ninja -C "$BUILDDIR/wlroots" install

# labwc dependencies
build_libxml2
build_libpng
build_freetype
build_fontconfig
build_pcre2
build_glib
build_fribidi
build_harfbuzz
build_freetype_pass2
build_cairo
build_pango

echo ""
echo "=== All dependencies built for $ABI ==="
echo "Sysroot: $PREFIX"
ls -lhS "$PREFIX/lib/"*.a 2>/dev/null || true
