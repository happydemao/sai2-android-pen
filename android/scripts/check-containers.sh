#!/system/bin/sh
H=/data/data/com.winlator/files/rootfs/home
echo "== home =="
ls -la "$H"
echo
for d in "$H"/xuser-*; do
  [ -d "$d" ] || continue
  C="$d/.wine/drive_c"
  echo "===== $d ====="
  ls -la "$C/windows/system32/wintab32.dll" 2>&1
  ls -la "$C"/wt_*.txt 2>&1
  echo "-- reg:"
  grep -c 'wintab32' "$d/.wine/user.reg" 2>/dev/null
  grep -c 'WT_DEBUG' "$d/.wine/user.reg" 2>/dev/null
  echo "-- SAI:"
  ls -d "$C"/*SAI* 2>&1
  echo
done
echo "== 正在运行的 wine/sai =="
ps -A -o PID,NAME | grep -i -E 'sai2|wine'
