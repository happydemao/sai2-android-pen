#!/system/bin/sh
D=/data/local/tmp/saitrace
F="$D/sai_30851.trace"

echo "===== any mention of .h.tmp ====="
grep -h 'h\.tmp' "$D"/*.trace | head -20

echo
echo "===== window 02:45:38.10 - 02:45:38.30, non-pipe syscalls ====="
awk '$2 >= "02:45:38.100000" && $2 <= "02:45:38.300000" { print }' "$F" | grep -v 'pipe:' | head -60

echo
echo "===== permissive-run error log (02:40) tail ====="
E="/data/data/com.winlator/files/rootfs/home/xuser-2/.wine/drive_c/users/xuser/Documents/SYSTEMAX Software Development/SAIv2/error"
tail -12 "$E/errlog.sai2.20261117.184027.txt"
echo "--- 02:42"
tail -8 "$E/errlog.sai2.20261117.184203.txt"
