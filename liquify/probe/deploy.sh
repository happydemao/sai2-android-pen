#!/bin/bash
# Deploy the GDI/canvas probe DLL into container-2 and restart SAI.
#
# Order matters: the DLL is copied *first*, then the package is stopped and
# relaunched. Copying after `am force-stop` raced with Winlator restarting the
# container on its own, which silently kept the previous probe build loaded.
#
# Usage: ./deploy.sh [dll]
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=/Users/happydemao/Documents/deepseek-harness/default-workspace
export HOME=$ROOT/.adb-home
ADB=$ROOT/android-platform-tools/adb
DLL=${1:-/private/tmp/wintab32-probe-94720.dll}
P=/data/data/lator.forsai/files/rootfs/home/xuser-2/.wine/drive_c
PKG=lator.forsai

$ADB shell "run-as $PKG sh -c 'cat > $P/windows/system32/wintab32.dll; printf wintab-2026-11-21-10 > $P/wt_bridge_ver.txt; rm -f $P/wt_gdi.log $P/wt_gdi_*.bmp'" < "$DLL"
$ADB shell "am force-stop $PKG" || true
sleep 2
$ADB shell "am force-stop $PKG" || true
$ADB shell "run-as $PKG ls -la $P/windows/system32/wintab32.dll"
"$HERE/launch_sai.sh"
