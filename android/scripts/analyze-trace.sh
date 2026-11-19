#!/system/bin/sh
D=/data/local/tmp/saitrace
echo "===== errno histogram ====="
grep -h '= -1 E' "$D"/*.trace | sed 's/.*= -1 //' | awk '{print $1}' | sort | uniq -c | sort -rn | head -20

echo
echo "===== file-related failures (excluding ENOENT) ====="
grep -h '= -1 E' "$D"/*.trace | grep -v ENOENT | tail -80

echo
echo "===== paths mentioned in failures ====="
grep -h '= -1 E' "$D"/*.trace | grep -v ENOENT | grep -o '"[^"]*"' | sort -u | tail -40
