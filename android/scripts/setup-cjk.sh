#!/system/bin/sh
# Install a CJK font into the Wine prefix and switch SAI UI language to Chinese.
set -e
C=/data/data/com.winlator/files/rootfs/home/xuser-2/.wine/drive_c
SAI="$C/SAI Ver.2"
T=/data/local/tmp/langwork
rm -rf "$T"; mkdir -p "$T"

# 1) CJK font
cp /system/fonts/NotoSansCJK-Regular.ttc "$C/windows/Fonts/NotoSansCJK-Regular.ttc"
chown 10314:10314 "$C/windows/Fonts/NotoSansCJK-Regular.ttc"
chmod 770 "$C/windows/Fonts/NotoSansCJK-Regular.ttc"

# 2) ini: UTF-16LE -> UTF-8, replace lang, back to UTF-16LE with BOM
iconv -f UTF-16 -t UTF-8 "$SAI/sai2.ini" | sed '1s/^\xEF\xBB\xBF//' > "$T/sai2.ini.u8"
sed -i 's/^lang[ \t]*=[ \t]*en[ \t]*$/lang = zh/' "$T/sai2.ini.u8"
printf '\377\376' > "$T/sai2.ini.new"
iconv -f UTF-8 -t UTF-16LE "$T/sai2.ini.u8" >> "$T/sai2.ini.new"
cp "$T/sai2.ini.new" "$SAI/sai2.ini"
chown 10314:10314 "$SAI/sai2.ini"
chmod 660 "$SAI/sai2.ini"

echo "== ini head =="
iconv -f UTF-16 -t UTF-8 "$SAI/sai2.ini" | head -4
echo "== font installed =="
ls -la "$C/windows/Fonts/NotoSansCJK-Regular.ttc"
