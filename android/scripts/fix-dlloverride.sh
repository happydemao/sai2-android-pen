#!/system/bin/sh
# 给当前容器补上 wintab32 的 native 覆盖（换容器后必做）
set -e
W=$(readlink -f /data/data/com.winlator/files/rootfs/home/xuser)
REG="$W/.wine/user.reg"

echo "container: $W"
cp "$REG" /data/local/tmp/user.reg.bak2

OWNER_U=$(stat -c %u "$REG")
OWNER_G=$(stat -c %g "$REG")

if grep -q '"wintab32"' "$REG"; then
  echo "already has wintab32 override:"
  grep -n '"wintab32"' "$REG"
else
  awk '/^\[Software.*DllOverrides\]/ && !done { print; print "\"wintab32\"=\"native,builtin\""; done=1; next } { print }' \
      "$REG" > /data/local/tmp/reg.new
  cat /data/local/tmp/reg.new > "$REG"
  echo "inserted"
  grep -n -A2 'DllOverrides' "$REG" | head -6
fi

chown "$OWNER_U:$OWNER_G" "$REG"
chmod 660 "$REG"
ls -la "$REG"
