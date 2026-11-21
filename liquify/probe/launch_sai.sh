#!/bin/bash
# Launch the SAI Ver.2 shortcut in container-2 and wait until SAI is really up.
#
# The Winlator "shortcuts" screen flips between landscape (2560x1600) and
# portrait (1600x2560), and the play button moves with it. Blind taps are how
# an earlier run opened Arknights from the Android launcher instead, so this
# script measures the current rotation before tapping and then proves SAI is
# alive by watching the DLL's status file become fresh.
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


A shell "monkey -p $PKG -c android.intent.category.LAUNCHER 1" >/dev/null 2>&1
sleep 6

W=$(A exec-out screencap -p > /tmp/ls_shot.png; python3 - <<'EOF'
import struct
d=open('/tmp/ls_shot.png','rb').read()
w,h=struct.unpack('>II', d[16:24])
print(w)
EOF
)
case "$W" in
  2560) XLIST="148,522" ;;
  2400) XLIST="591,632 291,638" ;;   # two known shortcut layouts on the phone
  1600) XLIST="231,549" ;;
  *)    XLIST="290,640" ;;
esac
echo "screen width $W -> tap candidates: $XLIST"
for XY in $XLIST; do
    X=${XY%,*}; Y=${XY#*,}
    A shell input tap "$X" "$Y"
    for i in $(seq 1 18); do
        sleep 2
        AGE=$(A shell "run-as $PKG sh -c 'echo \$(( \$(date +%s) - \$(stat -c %Y $P/wt_status.txt 2>/dev/null || echo 0) ))'" | tr -d '\r')
        if [ "${AGE:-999}" -lt 5 ] 2>/dev/null; then
            echo "SAI up (status ${AGE}s old) after tap $X,$Y"
            exit 0
        fi
    done
done
echo "SAI did not come up" >&2
A exec-out screencap -p > /private/tmp/launch_failed.png
exit 1
