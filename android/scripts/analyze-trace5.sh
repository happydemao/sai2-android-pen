#!/system/bin/sh
F=/data/local/tmp/saitrace/sai_30851.trace

echo "===== window 02:45:34.0 - 02:45:39.0 (hisdata + writes) ====="
awk '
  $2 >= "02:45:34.000000" && $2 <= "02:45:39.000000" {
    line=$0
    if (line ~ /hisdata/ || line ~ /(write|pwrite64|rename|mkdir|unlink|ftruncate|fsync)\(/) print line
  }' "$F" | grep -v 'wt_pressure\|wt_wake' | head -80

echo
echo "===== mmap/munmap mentioning hisdata ====="
grep 'hisdata' "$F" | grep -E 'mmap|munmap|msync' | head -20

echo
echo "===== every operation on hisdata dir (with results), that session ====="
grep 'hisdata/20261117.184514.2' "$F" | head -40
