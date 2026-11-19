#!/system/bin/sh
echo "== input devices =="
getevent -pl 2>/dev/null | grep -E '^add device|name:|  ABS_|  KEY_' | head -60
echo
echo "== dumpsys input: pen-ish devices =="
dumpsys input 2>/dev/null | grep -E 'Device [0-9]+:|Name:|Sources:|Classes:|Pointer' | head -60
echo
echo "== wt_status =="
C=/data/data/com.winlator/files/rootfs/home/xuser-1/.wine/drive_c
cat "$C/wt_status.txt" 2>&1
