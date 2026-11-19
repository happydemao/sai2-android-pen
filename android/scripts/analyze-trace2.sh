#!/system/bin/sh
D=/data/local/tmp/saitrace
F="$D/sai_30851.trace"

echo "===== errno histogram (all failures) ====="
grep -h ' = -1 ' "$F" | sed 's/.*= -1 //' | awk '{print $1}' | sort | uniq -c | sort -rn | head -20

echo
echo "===== failures on write-ish / rename / unlink / mkdir / mmap / fsync ====="
grep -h -E ' (write|pwrite64|ftruncate|fallocate|fsync|fdatasync|rename|renameat2?|unlink|unlinkat|mkdir|mkdirat|rmdir|mmap|munmap|msync|flock)\(' "$F" | grep ' = -1 ' | tail -40

echo
echo "===== EACCES/EPERM/ENOSPC/EDQUOT/ENOSYS anywhere ====="
grep -h -E '= -1 (EACCES|EPERM|ENOSPC|EDQUOT|ENOSYS)' "$F" | tail -40

echo
echo "===== last 30 failing lines before trace end ====="
grep -h ' = -1 ' "$F" | tail -30
