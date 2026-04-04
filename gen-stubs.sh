#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ABI="${ABI:-arm64-v8a}"

# Auto-detect NDK
if [ -z "${ANDROID_NDK_HOME:-}" ]; then
    SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}"
    if [ -n "$SDK" ] && [ -d "$SDK/ndk" ]; then
        ANDROID_NDK_HOME="$(ls -d "$SDK/ndk"/*/ 2>/dev/null | sort -V | tail -1)"
        ANDROID_NDK_HOME="${ANDROID_NDK_HOME%/}"
    fi
fi
NDK="${ANDROID_NDK_HOME:?ANDROID_NDK_HOME must be set}"
NM="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-nm"
PREFIX="$SCRIPT_DIR/sysroot/$ABI"

TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

# All undefined symbols from labwc + labnag matching our patterns
$NM --undefined-only "$SCRIPT_DIR/build/$ABI/labwc/liblabwc.a" "$SCRIPT_DIR/build/$ABI/labwc/clients/labnag" 2>/dev/null \
    | awk '{print $2}' \
    | grep -E '^(libinput_|wlr_backend_is_drm|wlr_drm_|wlr_libinput|wlr_output_is_drm|wlr_session_change_vt|wlr_input_device_is_libinput)' \
    | sort -u > "$TMPDIR/undef.txt"

# All defined symbols in wlroots AND labwc
{ $NM --defined-only "$PREFIX/lib/libwlroots-0.19.a" 2>/dev/null
  $NM --defined-only "$SCRIPT_DIR/build/$ABI/labwc/liblabwc.a" 2>/dev/null
} | awk '{print $3}' | sort -u > "$TMPDIR/defined.txt"

# Symbols we need to stub = undefined minus defined
comm -23 "$TMPDIR/undef.txt" "$TMPDIR/defined.txt" > "$TMPDIR/need.txt"

echo '/* Auto-generated Android stubs */'
echo '#include <stdbool.h>'
echo '#include <stddef.h>'
echo ''
while IFS= read -r sym; do
    echo "void *${sym}() { return 0; }"
done < "$TMPDIR/need.txt"
