#!/bin/bash
# Deploy the GDI survey probe into container-2 and restart SAI.
# Usage: ./deploy.sh [dll]
set -eu
ROOT=/Users/happydemao/Documents/deepseek-harness/default-workspace
export HOME=$ROOT/.adb-home
ADB=$ROOT/android-platform-tools/adb
DLL=${1:-/private/tmp/wintab32-probe-94720.dll}
P=/data/data/lator.forsai/files/rootfs/home/xuser-2/.wine/drive_c
PKG=lator.forsai

$ADB shell "am force-stop $PKG" || true
sleep 1
$ADB shell "run-as $PKG sh -c 'cat > $P/windows/system32/wintab32.dll; printf wintab-2026-11-21-10 > $P/wt_bridge_ver.txt; rm -f $P/wt_gdi.log $P/wt_gdi_*.bmp'" < "$DLL"
$ADB shell "run-as $PKG ls -la $P/windows/system32/wintab32.dll"
$ADB shell "monkey -p $PKG -c android.intent.category.LAUNCHER 1" >/dev/null 2>&1
sleep 4
$ADB shell input tap 167 507          # play button of the "SAI Ver.2" shortcut
sleep 30
echo "container up"
