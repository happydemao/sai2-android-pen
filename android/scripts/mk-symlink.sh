#!/system/bin/sh
C=/data/data/com.winlator/files/rootfs/home/xuser-2/.wine/drive_c
rm -f "$C/sai"
ln -s "SAI Ver.2" "$C/sai"
ls -la "$C/sai"
ls -la "$C/sai/sai2.exe"
