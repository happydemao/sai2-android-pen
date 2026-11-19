#!/system/bin/sh
E="/data/data/com.winlator/files/rootfs/home/xuser-2/.wine/drive_c/users/xuser/Documents/SYSTEMAX Software Development/SAIv2"
echo "== dir =="
ls -la "$E"
echo
echo "== error dir =="
ls -la "$E/error" 2>&1
echo
echo "== newest error files =="
ls -t "$E/error" 2>/dev/null | head -3 | while read f; do
  echo "----- $f"
  head -40 "$E/error/$f"
done
echo
echo "== hisdata =="
ls -la "$E/hisdata" 2>&1 | tail -10
