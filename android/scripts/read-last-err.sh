#!/system/bin/sh
E="/data/data/com.winlator/files/rootfs/home/xuser-2/.wine/drive_c/users/xuser/Documents/SYSTEMAX Software Development/SAIv2/error"
F="$E/errlog.sai2.20261117.185625.txt"
echo "== size =="; ls -la "$F"
echo "== lines with Path is invalid =="
grep -n 'Path is invalid' "$F"
echo "== context around first error =="
grep -n -B2 -A4 'Path is invalid' "$F" | head -30
echo "== tail =="
tail -12 "$F"
