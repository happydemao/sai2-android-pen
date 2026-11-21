#!/bin/bash
# Trigger the in-process memory survey and pull the results back.
# The probe waits up to 10 s for the next canvas repaint to lift a needle,
# then walks every readable private region looking for the same pixels.
set -u
ROOT=/Users/happydemao/Documents/deepseek-harness/default-workspace
export HOME=$ROOT/.adb-home
ADB=$ROOT/android-platform-tools/adb
P=/data/data/lator.forsai/files/rootfs/home/xuser-2/.wine/drive_c

$ADB shell "run-as lator.forsai sh -c 'rm -f $P/wt_memscan.log; touch $P/wt_memscan'"
echo "triggered; waiting for scan"
for i in $(seq 1 40); do
  sleep 1
  if $ADB shell "run-as lator.forsai test -s $P/wt_memscan.log" 2>/dev/null; then :; fi
  n=$($ADB shell "run-as lator.forsai cat $P/wt_memscan.log 2>/dev/null" | wc -l)
  if $ADB shell "run-as lator.forsai cat $P/wt_memscan.log 2>/dev/null" | grep -q "done:"; then
    echo "scan finished after ${i}s"
    break
  fi
done
$ADB exec-out "run-as lator.forsai cat $P/wt_memscan.log" | tee /private/tmp/wt_memscan.log
