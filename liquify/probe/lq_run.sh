#!/bin/bash
# Drive one full liquify round trip against the running SAI in container-2.
#
#   1. Ctrl+A / Ctrl+C   SAI copies the canvas at document resolution
#   2. wt_clipread       the DLL normalises the CF_DIB into wt_clipin.bin
#   3. wt_lq             applies one warp stroke from wt_lq.txt and publishes
#                        the result with wt_paste (clipboard)
#   4. Ctrl+V            SAI turns it into a new layer
#
# Usage: ./lq_run.sh [lq-spec]     e.g. "twirl_cw 800 800 800 800 250 0.8 40"
# Requires the probe DLL deployed and SAI in front of a document.
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
SPEC=${1:-"forward 700 800 900 800 200 0.8 40"}

# 1. copy the canvas
A shell input keycombination 113 29   # Ctrl+A
sleep 1
A shell input keycombination 113 31   # Ctrl+C
sleep 4
A shell input keycombination 113 32   # Ctrl+D (drop the selection)
sleep 1
A shell "run-as $PKG sh -c 'touch $P/wt_clipread'"
sleep 5
A exec-out "run-as $PKG sh -c 'cat $P/wt_clipin.txt'"

# 2. warp + publish
A shell "run-as $PKG sh -c 'printf \"$SPEC\" > $P/wt_lq.txt; touch $P/wt_lq'"
sleep 8
A exec-out "run-as $PKG sh -c 'grep LQ: $P/wt_memscan.log | tail -1'"

# 3. paste the result as a new layer
A shell input keycombination 113 50   # Ctrl+V
sleep 5
echo "pasted $SPEC"
