#!/system/bin/sh
C=/data/data/com.winlator/files/rootfs/home/xuser-2/.wine/drive_c
SAI="$C/SAI Ver.2"
echo "== 汉化说明.txt =="
cat "$SAI/汉化说明.txt" 2>&1 | head -60
echo
echo "== sai2.ini [Locale] =="
iconv -f UTF-16LE -t UTF-8 "$SAI/sai2.ini" 2>/dev/null | head -6
echo
echo "== wine Fonts dir =="
ls -la "$C/windows/Fonts" 2>&1 | head -20
echo
echo "== SAI dir tree (2 levels) =="
find "$SAI" -maxdepth 2 -type d 2>/dev/null
echo
echo "== device fonts (cjk) =="
ls -la /system/fonts/ | grep -i -E 'cjk|droid|noto' | head -20
