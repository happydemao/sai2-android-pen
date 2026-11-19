#!/system/bin/sh
B=/data/data/com.winlator/files/rootfs/home/xuser-2/.wine/drive_c
SAI="$B/SAI Ver.2"

echo "===== SAI dir ====="
ls -la "$SAI"

echo
echo "===== SAI tree (maxdepth 2) ====="
find "$SAI" -maxdepth 2 | head -60

echo
echo "===== ini files ====="
for f in "$SAI"/*.ini "$SAI"/init/*.ini; do
  [ -f "$f" ] && { echo "--- $f"; cat "$f"; }
done

echo
echo "===== users ====="
ls -la "$B/users"

echo
echo "===== profile dirs ====="
for u in "$B"/users/*; do
  echo "--- $u"
  ls -la "$u" 2>/dev/null | head -20
done

echo
echo "===== windows/temp ====="
ls -la "$B/windows/temp" 2>&1 | head -20
echo "--- local temp under profile"
for u in "$B"/users/*; do
  ls -la "$u/Temp" 2>&1 | head -10
  ls -la "$u/AppData/Local/Temp" 2>&1 | head -10
done

echo
echo "===== wine logs ====="
ls -la "$B/../" | head -20
