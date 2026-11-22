#!/bin/bash
# Put the mscms proxy carrier next to sai2.exe inside the Winlator container.
#
#   mscms.dll       our proxy (forwards every colour-management export)
#   mscms_orig.dll  Wine's real mscms, renamed so the forwarders resolve
#
# Usage: SERIAL=... PKG=lator.forsai USERDIR=xuser ./deploy_carrier.sh [/path/to/SAI dir]
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=/Users/happydemao/Documents/deepseek-harness/default-workspace
export HOME=$ROOT/.adb-home
ADB=$ROOT/android-platform-tools/adb
SERIAL=${SERIAL:-78421d9f}
PKG=${PKG:-lator.forsai}
USERDIR=${USERDIR:-xuser}
CD=/data/data/$PKG/files/rootfs/home/$USERDIR/.wine/drive_c
SAI_DIR=${1:-}
A() { "$ADB" -s "$SERIAL" "$@"; }

if [ -z "$SAI_DIR" ]; then
    # first directory that holds sai2.exe
    SAI_DIR=$(A exec-out "run-as $PKG sh -c 'for d in $CD/*/; do [ -f \"\${d}sai2.exe\" ] && echo \$d && break; done'" | tr -d '\r')
fi
[ -n "$SAI_DIR" ] || { echo "sai2.exe not found under $CD" >&2; exit 1; }
echo "SAI dir: $SAI_DIR"

# 1. Wine's mscms becomes mscms_orig.dll in the SAI directory
A shell "run-as $PKG sh -c 'cp $CD/windows/system32/mscms.dll \"$SAI_DIR/mscms_orig.dll\"'" 
# 2. our proxy
A shell "run-as $PKG sh -c 'cat > \"$SAI_DIR/mscms.dll\"'" < "$HERE/mscms.dll"
A shell "run-as $PKG sh -c 'rm -f $CD/mscms_carrier.log'"
A shell "run-as $PKG sh -c 'ls -la \"$SAI_DIR/mscms\"*.dll'"
echo "carrier deployed; start SAI and check C:\\mscms_carrier.log"
