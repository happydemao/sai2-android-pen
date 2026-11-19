#!/system/bin/sh
W=/data/data/com.winlator/files/rootfs/opt/wine
echo "== lib/wine dirs =="
ls -la "$W/lib/wine"
echo
echo "== unix ntdll =="
for f in "$W"/lib/wine/*-unix/ntdll.so; do
  echo "--- $f"
  ls -la "$f"
  head -c 20 "$f" | od -An -tx1 | head -2
done
echo
echo "== wine loader =="
for f in "$W/bin/wine" "$W/bin/wine64" "$W/bin/wineserver"; do
  [ -f "$f" ] && { ls -la "$f"; head -c 20 "$f" | od -An -tx1 | head -2; }
done
echo
echo "== version =="
cat "$W/share/wine/wine.inf" 2>/dev/null | head -5
grep -r "Version" "$W/share/wine/wine.inf" 2>/dev/null | head -5
