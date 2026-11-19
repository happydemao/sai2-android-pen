#!/system/bin/sh
C=/data/data/com.winlator/files/rootfs/home/xuser-2/.wine/drive_c
SAI="$C/SAI Ver.2"
cp /data/local/tmp/sai2.ini.new "$SAI/sai2.ini"
chown 10314:10314 "$SAI/sai2.ini"
chmod 660 "$SAI/sai2.ini"
echo "== head =="
iconv -f UTF-16 -t UTF-8 "$SAI/sai2.ini" 2>/dev/null | head -4
ls -la "$SAI/sai2.ini"
