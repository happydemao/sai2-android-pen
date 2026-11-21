#!/bin/bash
# Open a new 1600x1600 canvas in the running SAI, dismissing the crash-recovery dialog.
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


A shell input tap 1057 646          # 否(N) on "强制退出的工程"
sleep 2
A shell input keycombination 113 42 # Ctrl+N
sleep 3
A shell input tap 787 528           # width field
sleep 1
A shell input keycombination 113 29 # Ctrl+A
sleep 1
A shell input text 1600
sleep 1
A shell input tap 787 574           # height field
sleep 1
A shell input keycombination 113 29
sleep 1
A shell input text 1600
sleep 1
A shell input keyevent 66           # Enter -> create
sleep 6
A exec-out screencap -p > /private/tmp/opened.png
echo "opened"
