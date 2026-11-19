#!/system/bin/sh
C=/data/data/com.winlator/files/rootfs/home/xuser-1/.wine/drive_c
echo "== drive_c =="
ls -la "$C" 2>&1
echo
echo "== system32/wintab32.dll =="
ls -la "$C/windows/system32/wintab32.dll" 2>&1
echo
echo "== wt files =="
ls -la "$C"/wt_*.txt 2>&1
echo
echo "== SAI dirs =="
ls -d "$C"/*SAI* 2>&1
echo
echo "== reg: wintab32 / WT_DEBUG / Fonts Replacements =="
grep -n wintab32 "$C/../user.reg" 2>&1
grep -n WT_DEBUG "$C/../user.reg" 2>&1
grep -n 'Replacements' "$C/../user.reg" 2>&1
echo
echo "== fonts dir (cjk?) =="
ls -la "$C/windows/Fonts" 2>/dev/null | grep -i noto
echo
echo "== running procs =="
ps -A -o PID,NAME | grep -i -E 'sai2|wine' | head
