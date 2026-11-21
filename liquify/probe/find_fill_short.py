#!/usr/bin/env python3
"""Second pass: SAI's tile grid is finer than 64px, so shorten the needle.

A solid-colour layer gives a run of identical pixels as long as the storage's
innermost row. If tiles are 64 wide the run is 256 B, 32 wide -> 128 B,
16 wide -> 64 B. The first sweep tried 250 B and found nothing outside the
probe's own buffer, so step down and watch which length starts hitting.
"""
import os
import struct
import subprocess
import sys

ROOT = "/Users/happydemao/Documents/deepseek-harness/default-workspace"
ADB = os.path.join(ROOT, "android-platform-tools", "adb")
P = "/data/data/lator.forsai/files/rootfs/home/xuser-2/.wine/drive_c"
ENV = dict(os.environ, HOME=os.path.join(ROOT, ".adb-home"))
SELF_REGION = "0F932000"      # the probe's own static buffer: never a real hit


def adb(*args, **kw):
    return subprocess.run([ADB, *args], env=ENV, check=True, capture_output=True, **kw)


def sh(cmd):
    return adb("shell", f"run-as lator.forsai sh -c '{cmd}'").stdout


def scan(pat, label):
    open("/private/tmp/lq_pat.bin", "wb").write(pat)
    sh(f"rm -f {P}/wt_memscan.log {P}/wt_pattern.bin")
    adb("shell", f"run-as lator.forsai sh -c 'cat > {P}/wt_pattern.bin'",
        input=pat)
    sh(f"touch {P}/wt_memscan")
    log = ""
    for _ in range(45):
        subprocess.run(["sleep", "2"], check=True)
        log = adb("exec-out", f"run-as lator.forsai sh -c 'cat {P}/wt_memscan.log'").stdout.decode("utf-8", "replace")
        if "done:" in log:
            break
    hits = [l for l in log.splitlines() if l.startswith("HIT")]
    real = [l for l in hits if SELF_REGION not in l]
    print(f"{label:26s} hits={len(hits):2d}  real={len(real)}")
    for l in real[:8]:
        print("   ", l)
    sh(f"rm -f {P}/wt_pattern.bin")
    return real


def main():
    # rgb(255,64,0) as sampled from the screen; SAI shows it as pure BGR storage
    r, g, b = 255, 64, 0
    orders = {
        "BGRA": bytes((b, g, r, 0xFF)),
        "RGBA": bytes((r, g, b, 0xFF)),
        "BGRX0": bytes((b, g, r, 0x00)),
        "BGR24": bytes((b, g, r)),
        "RGB24": bytes((r, g, b)),
    }
    total = []
    for name, unit in orders.items():
        for px in (16, 8, 4):
            pat = unit * px
            total += scan(pat, f"{name} x{px}px ({len(pat)}B)")
    print("\nnon-empty results:")
    for l in total:
        print("  ", l)


if __name__ == "__main__":
    sys.exit(main())
