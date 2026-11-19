#!/system/bin/sh
# Make Wine substitute a CJK font for the UI fonts (Tahoma / MS Shell Dlg ...).
W=/data/data/com.winlator/files/rootfs/home/xuser-2/.wine
REG="$W/user.reg"
MARK='[Software\\\\Wine\\\\Fonts\\\\Replacements]'

cp "$REG" /data/local/tmp/user.reg.bak

if grep -q 'Fonts\\\\\\\\Replacements' "$REG"; then
  echo "section already present, leaving as is"
else
  # .reg format: section header then "name"="value" lines, blank line terminated
  cat >> "$REG" <<'EOF'

[Software\\Wine\\Fonts\\Replacements] 1700000000
"MS Shell Dlg"="Noto Sans CJK SC"
"MS Shell Dlg 2"="Noto Sans CJK SC"
"Tahoma"="Noto Sans CJK SC"
"MS Sans Serif"="Noto Sans CJK SC"
"SimSun"="Noto Sans CJK SC"
"Microsoft YaHei"="Noto Sans CJK SC"

EOF
  echo "appended replacements"
fi

chown 10314:10314 "$REG"
chmod 660 "$REG"
tail -12 "$REG"
