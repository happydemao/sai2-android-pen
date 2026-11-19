#!/system/bin/sh
F=/data/local/tmp/saitrace/sai_30851.trace

echo "===== hisdata / SAIv2 activity (non-ENOENT probes) ====="
grep -h 'hisdata\|SAIv2' "$F" | grep -v 'ENOENT' | tail -40

echo
echo "===== trace window ====="
head -1 "$F"
tail -1 "$F"

echo
echo "===== count of EACCES per second ====="
grep -h 'EACCES' "$F" | awk '{print $2}' | cut -c1-8 | uniq -c | tail -20
