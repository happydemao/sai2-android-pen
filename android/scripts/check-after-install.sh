#!/system/bin/sh
C=/data/data/com.winlator/files/rootfs/home/xuser-2/.wine/drive_c
SAI="$C/SAI Ver.2"
echo "== SAI dir =="
ls -la "$SAI" | head -30
echo
echo "== wintab32 in system32 =="
ls -la "$C/windows/system32/wintab32.dll" 2>&1
echo
echo "== wt_status =="
ls -la "$C"/wt_*.txt 2>&1
cat "$C/wt_status.txt" 2>&1
echo
echo "== user.reg DllOverrides / Environment =="
grep -A3 'DllOverrides' "$C/../user.reg" | head -10
grep -A3 '\[Environment\]' "$C/../user.reg" | head -10
