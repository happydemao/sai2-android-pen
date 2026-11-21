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
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=/Users/happydemao/Documents/deepseek-harness/default-workspace
export HOME=$ROOT/.adb-home
ADB=$ROOT/android-platform-tools/adb
P=/data/data/lator.forsai/files/rootfs/home/xuser-2/.wine/drive_c
SPEC=${1:-"forward 700 800 900 800 200 0.8 40"}

# 1. copy the canvas
$ADB shell input keycombination 113 29   # Ctrl+A
sleep 1
$ADB shell input keycombination 113 31   # Ctrl+C
sleep 4
$ADB shell input keycombination 113 32   # Ctrl+D (drop the selection)
sleep 1
$ADB shell "run-as lator.forsai sh -c 'touch $P/wt_clipread'"
sleep 5
$ADB exec-out "run-as lator.forsai sh -c 'cat $P/wt_clipin.txt'"

# 2. warp + publish
$ADB shell "run-as lator.forsai sh -c 'printf \"$SPEC\" > $P/wt_lq.txt; touch $P/wt_lq'"
sleep 8
$ADB exec-out "run-as lator.forsai sh -c 'grep LQ: $P/wt_memscan.log | tail -1'"

# 3. paste the result as a new layer
$ADB shell input keycombination 113 50   # Ctrl+V
sleep 5
echo "pasted $SPEC"
