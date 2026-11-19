#!/system/bin/sh
cd "/data/data/com.winlator/files/rootfs/home/xuser-2/.wine/drive_c" || exit 1
D="SAI Ver.2"

echo "===== sai2.ini (utf-16) ====="
iconv -f UTF-16LE -t UTF-8 "$D/sai2.ini" 2>/dev/null | tr -d '\r' | grep -v '^$'
echo
echo "===== raw utf16 tail ====="
iconv -f UTF-16LE -t UTF-8 "$D/sai2.ini" 2>/dev/null | tr -d '\r' | tail -40
echo
echo "===== sai2.json ====="
cat "$D/sai2.json"
