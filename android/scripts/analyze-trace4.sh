#!/system/bin/sh
D=/data/local/tmp/saitrace
for F in "$D"/sai_*.trace; do
  echo "===== $F ($(wc -l < "$F") lines) ====="
  echo -n "hisdata: "; grep -c 'hisdata' "$F"
  echo -n "SAIv2: ";   grep -c 'SAIv2' "$F"
  echo -n "history: "; grep -ci 'histor' "$F"
  echo "--- first non-ENOENT hisdata/SAIv2 lines:"
  grep -i 'hisdata\|saiv2' "$F" | grep -v ENOENT | head -15
  echo "--- openat results containing SAI:"
  grep 'openat' "$F" | grep -i 'sai' | tail -10
done
