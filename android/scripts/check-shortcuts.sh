#!/system/bin/sh
C=/data/data/com.winlator/files/rootfs/home/xuser-2/.wine/drive_c
echo "== user Desktop =="
ls -la "$C/users/xuser/Desktop"
echo
echo "== Public Desktop =="
ls -la "$C/users/Public/Desktop"
echo
echo "== user Desktop .desktop content =="
for f in "$C/users/xuser/Desktop/"*.desktop; do
  [ -f "$f" ] && { echo "--- $f"; cat "$f"; }
done
