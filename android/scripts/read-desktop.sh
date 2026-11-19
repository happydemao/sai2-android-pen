#!/system/bin/sh
D=/data/data/com.winlator/files/rootfs/home/xuser-2/.wine/drive_c/users/xuser/Desktop
echo "== files =="
ls -la "$D"
echo
echo "== SAI Ver.2.desktop =="
cat "$D/SAI Ver.2.desktop"
echo
echo "== icon dir? =="
ls -la /data/data/com.winlator/files/rootfs/home/xuser-2/.winlator 2>/dev/null | head
find /data/data/com.winlator/files/rootfs/home/xuser-2 -maxdepth 3 -name '*.png' 2>/dev/null | head
