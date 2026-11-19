#!/system/bin/sh
C=/data/data/com.winlator/files/rootfs/home/xuser-2/.wine/drive_c
SAI="$C/SAI Ver.2"
cp "$SAI/sai2.ini" /data/local/tmp/sai2.ini
chmod 644 /data/local/tmp/sai2.ini
ls -la /data/local/tmp/sai2.ini
