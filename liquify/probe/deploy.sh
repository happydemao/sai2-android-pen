#!/bin/bash
# Deploy the GDI/canvas probe DLL into container-2 and restart SAI.
#
# Order matters: the DLL is copied *first*, then the package is stopped and
# relaunched. Copying after `am force-stop` raced with Winlator restarting the
# container on its own, which silently kept the previous probe build loaded.
#
# Usage: ./deploy.sh [dll]
set -eu
ROOT=/Users/happydemao/Documents/deepseek-harness/default-workspace
export HOME=$ROOT/.adb-home
ADB=$ROOT/android-platform-tools/adb
SERIAL=${SERIAL:-}
PKG=${PKG:-lator.forsai}
USERDIR=${USERDIR:-xuser}
P=${P:-/data/data/$PKG/files/rootfs/home/$USERDIR/.wine/drive_c}
A() { "$ADB" ${SERIAL:+-s "$SERIAL"} "$@"; }
SH() { A shell "run-as $PKG sh -c '$*'"; }

HERE=$(cd "$(dirname "$0")" && pwd)
DLL=${1:-/private/tmp/wintab32-probe-94720.dll}

A shell "run-as $PKG sh -c 'cat > $P/windows/system32/wintab32.dll; printf wintab-2026-11-21-10 > $P/wt_bridge_ver.txt; rm -f $P/wt_gdi.log $P/wt_gdi_*.bmp'" < "$DLL"
export SERIAL PKG USERDIR P
A shell "am force-stop $PKG" || true
sleep 2
A shell "am force-stop $PKG" || true
# Winlator's container runs under proot and can outlive force-stop; SAI would
# keep the previous wintab32.dll loaded, silently ignoring the new build.
for i in 1 2 3; do
    sleep 2
    PIDS=$(A shell "run-as $PKG pidof sai2.exe 2>/dev/null" | tr -d '\r')
    [ -z "$PIDS" ] && break
    for pid in $PIDS; do
        A shell "run-as $PKG kill -9 $pid" 2>/dev/null || true
    done
done
A shell "run-as $PKG ls -la $P/windows/system32/wintab32.dll"
"$HERE/launch_sai.sh"
