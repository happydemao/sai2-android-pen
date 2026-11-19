#!/system/bin/sh
W=/data/data/com.winlator/files/rootfs/home/xuser-2/.wine
echo "== dosdevices =="
ls -la "$W/dosdevices"
echo
echo "== drive_c =="
ls -la "$W/drive_c"
echo
echo "== Program Files =="
ls -la "$W/drive_c/Program Files" | head -20
echo
echo "== any sai2.exe under prefix =="
find "$W/drive_c" -maxdepth 3 -iname 'sai2.exe' 2>/dev/null
echo
echo "== newest files in drive_c (top 15 by mtime) =="
ls -lt "$W/drive_c"/* 2>/dev/null | head -15
echo
echo "== /tmp sai things =="
ls -la /data/data/com.winlator/files/rootfs/tmp 2>/dev/null | head -15
