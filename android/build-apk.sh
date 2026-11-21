#!/bin/bash
# Rebuild the lator.forsai APK with the Android-side liquify server.
#
# Pipeline (macOS, no pwsh / no Gradle):
#   1. apktool d -r  the installed APK (or a dist APK) -> smali only
#   2. inject `LiquifyServer.start()` into TouchpadView.updateXform smali
#   3. apktool b    rebuild (classes.dex changes, resources copied)
#   4. javac + d8   compile LiquifyWarp/LiquifyServer -> classes3.dex
#   5. zip add classes3.dex, zipalign, apksigner (keys/debug.keystore)
#   6. adb install -r
#
# The keystore MUST be the one the installed app was signed with
# (keys/debug.keystore, SHA-256 02:01:22:...), otherwise the update is
# rejected and the container data is lost.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
ROOT=$(cd "$REPO/.." && pwd)
SDK=$ROOT/tools/android-sdk
JDK=$ROOT/tools/jdk/jdk-21.0.13+9/Contents/Home
BT=$SDK/build-tools/35.0.0
AJAR=$SDK/platforms/android-35/android.jar
APKTOOL=$ROOT/tools/bin/apktool.jar
KEYSTORE=$ROOT/keys/debug.keystore
WORK=/private/tmp/liquify-apk
mkdir -p /private/tmp/apkhome

DEVICE=${DEVICE:-}
BASE=${1:-}
if [ -z "$BASE" ] && [ -f "$REPO/Winlator-for-sai-0.3.30-lator.forsai.apk" ]; then
    # Always start from a clean release build: decoding an APK that already
    # contains LiquifyServer would bake the old Java classes into the smali
    # tree and the rebuilt classesN.dex would shadow the freshly compiled ones.
    BASE=$REPO/Winlator-for-sai-0.3.30-lator.forsai.apk
fi
if [ -z "$BASE" ]; then
    ADB=$ROOT/android-platform-tools/adb
    PKG=${PKG:-lator.forsai}
    ADB_APK=$(env HOME="$ROOT/.adb-home" "$ADB" ${DEVICE:+-s "$DEVICE"} shell pm path $PKG | sed 's/package://' | tr -d '\r')
    env HOME="$ROOT/.adb-home" "$ADB" ${DEVICE:+-s "$DEVICE"} exec-out "cat '$ADB_APK'" > "$WORK-base.apk"
    BASE=$WORK-base.apk
fi

rm -rf "$WORK" && mkdir -p "$WORK"
echo "== decode"
env HOME=/private/tmp/apkhome java -jar "$APKTOOL" d -f -r -o "$WORK/dec" "$BASE" > "$WORK/decode.log" 2>&1

echo "== drop stale liquify classes from the decoded tree"
find "$WORK/dec" -name 'Liquify*.smali' -delete

echo "== inject smali"
python3 - "$WORK/dec" <<'PY'
import sys, pathlib
dec = pathlib.Path(sys.argv[1])
p = dec / "smali/com/winlator/widget/TouchpadView.smali"
s = p.read_text()
needle = "    invoke-static {p3, p4}, Lcom/winlator/sai/PenBridge;->configure(II)V\n"
add = "    invoke-static {}, Lcom/winlator/sai/LiquifyServer;->start()V\n"
if add.strip() not in s:
    assert needle in s, "configure call site not found"
    s = s.replace(needle, needle + "\n" + add, 1)
    p.write_text(s)
    print("injected into", p.name)
else:
    print("already injected")
PY

echo "== apktool build"
env HOME=/private/tmp/apkhome java -jar "$APKTOOL" b -o "$WORK/rebuilt.apk" "$WORK/dec" > "$WORK/build.log" 2>&1

echo "== javac + d8"
mkdir -p "$WORK/classes" "$WORK/dex"
"$JDK/bin/javac" -nowarn -source 8 -target 8 -bootclasspath "$AJAR" \
    -d "$WORK/classes" "$HERE/LiquifyWarp.java" "$HERE/LiquifyServer.java"
"$BT/d8" --release --min-api 26 --lib "$AJAR" --output "$WORK/dex" \
    $(find "$WORK/classes" -name '*.class')

echo "== pack dex"
python3 - "$WORK" <<'PY'
import sys, zipfile, shutil, pathlib, re, os
work = pathlib.Path(sys.argv[1])
src = work / "rebuilt.apk"
# apktool may already emit classes3.dex etc.; append ours under the next free
# index instead of duplicating a name (apksigner rejects duplicates).
n = 1
with zipfile.ZipFile(src) as z:
    names = z.namelist()
for nm in names:
    m = re.fullmatch(r"classes(\d+)\.dex", nm)
    if m:
        n = max(n, int(m.group(1)))
out = work / "withdex.apk"
with zipfile.ZipFile(src) as zin, zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as zout:
    for item in zin.infolist():
        data = zin.read(item.filename)
        zout.writestr(item, data)
    zout.writestr(f"classes{n + 1}.dex", (work / "dex/classes.dex").read_bytes())
print(f"added classes{n + 1}.dex (existing dex files: {[x for x in names if x.endswith('.dex')]})")
PY

echo "== align + sign"
"$BT/zipalign" -f -p 4 -P 16 "$WORK/withdex.apk" "$WORK/aligned.apk"
"$BT/apksigner" sign --ks "$KEYSTORE" --ks-pass pass:android \
    --ks-key-alias androiddebugkey --out "$WORK/signed.apk" "$WORK/aligned.apk" 2>/dev/null
"$BT/apksigner" verify --print-certs "$WORK/signed.apk" | head -2

echo "== install"
ADB=$ROOT/android-platform-tools/adb
env HOME="$ROOT/.adb-home" "$ADB" ${DEVICE:+-s "$DEVICE"} push "$WORK/signed.apk" /data/local/tmp/liquify-patched.apk >/dev/null
env HOME="$ROOT/.adb-home" "$ADB" ${DEVICE:+-s "$DEVICE"} shell "pm install -r -t /data/local/tmp/liquify-patched.apk" | tail -2
echo "done: $WORK/signed.apk"
