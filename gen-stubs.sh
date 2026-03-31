#!/bin/bash
set -euo pipefail
NDK="${ANDROID_NDK_HOME:-/home/ian/Android/Sdk/ndk/28.2.13676358}"
PREFIX="${1:-sysroot/arm64-v8a}"
NM="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-nm"

TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

# All undefined symbols from labwc + labnag matching our patterns
$NM --undefined-only build/arm64-v8a/labwc/liblabwc.a build/arm64-v8a/labwc/clients/labnag 2>/dev/null \
    | awk '{print $2}' \
    | grep -E '^(libinput_|wlr_backend_is_drm|wlr_drm_|wlr_libinput|wlr_output_is_drm|wlr_session_change_vt|wlr_input_device_is_libinput)' \
    | sort -u > "$TMPDIR/undef.txt"

# All defined symbols in wlroots AND labwc
{ $NM --defined-only "$PREFIX/lib/libwlroots-0.19.a" 2>/dev/null
  $NM --defined-only build/arm64-v8a/labwc/liblabwc.a 2>/dev/null
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
