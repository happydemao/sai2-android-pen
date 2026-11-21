#!/bin/bash
# Prepare a 1600x1600 document with the test image on the 2400x1080 phone.
# Coordinates were measured on the Redmi K40 Pro container-1 window.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=/Users/happydemao/Documents/deepseek-harness/default-workspace
export HOME=$ROOT/.adb-home
ADB=$ROOT/android-platform-tools/adb
SERIAL=${SERIAL:-78421d9f}
PKG=${PKG:-lator.forsai}
USERDIR=${USERDIR:-xuser}
P=${P:-/data/data/$PKG/files/rootfs/home/$USERDIR/.wine/drive_c}
A() { "$ADB" ${SERIAL:+-s "$SERIAL"} "$@"; }

# crash-recovery dialog (ignore failure when it is not shown)
A shell input tap 1008 464 || true
sleep 2
# Ctrl+N, width 1600, height 1600
A shell input keycombination 113 42
sleep 3
A shell input tap 759 343
sleep 1
A shell input keycombination 113 29
A shell input text "1600"
sleep 1
A shell input tap 759 387
sleep 1
A shell input keycombination 113 29
A shell input text "1600"
sleep 1
A shell input tap 1057 677
sleep 5

# paste the test image (green square + red diagonal)
A shell "run-as $PKG sh -c 'cat > $P/wt_paste.bin'" < /private/tmp/paste1600.bin
A shell "run-as $PKG sh -c 'printf \"1600 1600\" > $P/wt_paste.txt; touch $P/wt_paste'"
sleep 3
A shell input keycombination 113 50
sleep 5
echo "document ready"
