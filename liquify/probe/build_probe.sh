#!/bin/bash
# Build the canvas/GDI probe DLL for the lator.forsai container.
#
# The app's PenBridge.ensureBridgeFiles() only leaves an existing DLL alone
# when its *file length* equals the embedded release size (94720) AND the
# version stamp matches. So the probe build is stripped and zero-padded to
# exactly that size; the padding after the PE image is ignored by the loader.
#
# Toolchain: LLVM-mingw. Set MINGW_BIN if it is not in /private/tmp (see
# docs/20-canvas-capture.md; the tarball is llvm-mingw-*-ucrt-macos-universal).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
MINGW_BIN=${MINGW_BIN:-/private/tmp/llvm-mingw/llvm-mingw-20261117-ucrt-macos-universal/bin}
OUT=${1:-/private/tmp/wintab32-probe-94720.dll}
PAD=94720

"$MINGW_BIN/x86_64-w64-mingw32-gcc" -shared -Oz -ffunction-sections -fdata-sections \
    -Wl,--gc-sections -o /tmp/wt-probe-raw.dll \
    "$HERE/wintab32_probe.c" "$HERE/../engine/lq.c" "$HERE/wintab32.def" \
    -I "$HERE" -I "$ROOT/bridge" \
    -lgdi32 -luser32 -lws2_32 -municode
"$MINGW_BIN/llvm-strip" --strip-unneeded /tmp/wt-probe-raw.dll -o /tmp/wt-probe-stripped.dll

SIZE=$(stat -f%z /tmp/wt-probe-stripped.dll)
if [ "$SIZE" -gt "$PAD" ]; then
    echo "probe DLL is $SIZE bytes, cannot pad down to $PAD" >&2
    exit 1
fi
cp /tmp/wt-probe-stripped.dll "$OUT"
dd if=/dev/zero bs=1 count=$((PAD - SIZE)) >> "$OUT" 2>/dev/null
echo "built $OUT ($(stat -f%z "$OUT") bytes, image $SIZE)"
