#!/system/bin/sh
F=/data/local/tmp/saitrace/sai_30851.trace

echo "=== fsync / pwrite / pread lines on .h.tmp ==="
grep -h 'h\.tmp' "$F" | grep -E 'resumed|= [0-9-]' | head -20

echo
echo "=== all EINVAL lines ==="
grep -h 'EINVAL' "$F" | head -20

echo
echo "=== fsync / fdatasync anywhere with result ==="
grep -h -E '(fsync|fdatasync)\(' "$F" | tail -15

echo
echo "=== pwrite64 completed results ==="
grep -h 'pwrite64.*resumed\|pwrite64.*= [0-9-]' "$F" | tail -10

echo
echo "=== 02:45:38.12 - 38.17 all syscalls (incl. unfinished/resumed) ==="
awk '$2 >= "02:45:38.120000" && $2 <= "02:45:38.175000" { print }' "$F" | grep -v 'pipe:\|eventfd'
