#!/system/bin/sh
# Wait for a NEW sai2.exe, then trace the syscalls that matter for file I/O failures.
OUT=/data/local/tmp/saitrace
mkdir -p "$OUT"
rm -f "$OUT"/*.trace "$OUT"/log

SYSCALLS=openat,open,creat,write,pwrite64,read,pread64,ftruncate,fallocate,fsync,fdatasync,rename,renameat,renameat2,unlink,unlinkat,mkdir,mkdirat,rmdir,ioctl,mmap,munmap,msync,statx,newfstatat,getdents64,flock,fcntl

last=""
i=0
while [ $i -lt 1200 ]; do
  p=$(pidof sai2.exe 2>/dev/null | awk '{print $1}')
  if [ -n "$p" ] && [ "$p" != "$last" ]; then
    last=$p
    echo "attached $p at $(date +%H:%M:%S)" >> "$OUT/log"
    strace -f -tt -y -s 200 -o "$OUT/sai_$p.trace" -e trace=$SYSCALLS -p "$p" >/dev/null 2>&1 &
  fi
  i=$((i + 1))
  sleep 0.5
done
echo "watch ended" >> "$OUT/log"
