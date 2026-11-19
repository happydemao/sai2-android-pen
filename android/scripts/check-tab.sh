#!/system/bin/sh
H=/data/data/com.winlator/files/rootfs/home
echo "== containers =="
ls -la "$H" 2>&1
echo
C=""
for d in "$H"/xuser-*; do [ -d "$d" ] && C="$d"; break; done
echo "container: $C"
echo
echo "== drive_c =="
ls -la "$C/.wine/drive_c" 2>&1
echo
echo "== wt files =="
ls -la "$C/.wine/drive_c"/wt_*.txt 2>&1
cat "$C/.wine/drive_c/wt_status.txt" 2>&1
echo
echo "== wintab32.dll =="
ls -la "$C/.wine/drive_c/windows/system32/wintab32.dll" 2>&1
echo
echo "== SAI dir =="
ls -d "$C/.wine/drive_c"/*SAI* 2>&1
echo
echo "== registry =="
grep -n wintab32 "$C/.wine/user.reg" 2>&1
grep -n WT_DEBUG "$C/.wine/user.reg" 2>&1
grep -n Replacements "$C/.wine/user.reg" 2>&1
echo
echo "== 输入设备（S Pen）=="
dumpsys input 2>/dev/null | grep -A6 -i 'pen\|stylus' | head -40
