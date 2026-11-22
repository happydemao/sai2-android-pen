#!/bin/bash
# Build the mscms.dll proxy carrier (x86_64, Windows).
# Needs llvm-mingw; override MINGW_BIN when it lives elsewhere.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
MINGW_BIN=${MINGW_BIN:-/private/tmp/llvm-mingw/bin}
CC=$MINGW_BIN/x86_64-w64-mingw32-gcc
"$CC" -shared -O2 -o "$HERE/mscms.dll" "$HERE/mscms_proxy.c" "$HERE/mscms.def" \
      -Wl,--kill-at -static-libgcc
"$MINGW_BIN/llvm-strip" "$HERE/mscms.dll" 2>/dev/null || true
ls -la "$HERE/mscms.dll"
