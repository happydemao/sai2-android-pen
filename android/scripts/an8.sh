#!/system/bin/sh
E="/data/data/com.winlator/files/rootfs/home/xuser-2/.wine/drive_c/users/xuser/Documents/SYSTEMAX Software Development/SAIv2/error"
for f in "$E"/*.txt; do
  n=$(grep -c 'Path is invalid' "$f")
  echo "$n  $(basename "$f")"
done
