#!/usr/bin/env python3
"""Find SAI's layer storage by hunting for a solid fill colour in memory.

Protocol
--------
1. The user fills the canvas with one saturated colour (bucket tool).
2. We take a screenshot and read the colour from the middle of the canvas.
3. We push a pattern file holding N repetitions of that pixel in every byte
   order SAI might plausibly use, trigger the in-process scanner, and look at
   which regions light up.

A solid 1600x1600 layer is 2.56 M identical pixels: whatever layout SAI uses
(plain BGRA, RGBA, 16 bit per channel, or tiled) a long run of the same pixel
survives, so this discriminates between formats without knowing them.
"""
import struct
import subprocess
import sys
import zlib
import os

ROOT = "/Users/happydemao/Documents/deepseek-harness/default-workspace"
ADB = os.path.join(ROOT, "android-platform-tools", "adb")
P = "/data/data/lator.forsai/files/rootfs/home/xuser-2/.wine/drive_c"
ENV = dict(os.environ, HOME=os.path.join(ROOT, ".adb-home"))


def adb(*args, **kw):
    return subprocess.run([ADB, *args], env=ENV, check=True, **kw)


def shell(cmd):
    return adb("shell", f"run-as lator.forsai sh -c '{cmd}'", capture_output=True).stdout


def read_png(path):
    d = open(path, "rb").read()
    assert d[:8] == b"\x89PNG\r\n\x1a\n"
    pos, idat = 8, b""
    w = h = ct = bd = 0
    while pos < len(d):
        ln = struct.unpack(">I", d[pos:pos + 4])[0]
        typ = d[pos + 4:pos + 8]
        data = d[pos + 8:pos + 8 + ln]
        pos += 12 + ln
        if typ == b"IHDR":
            w, h, bd, ct = struct.unpack(">IIBB", data[:10])
        elif typ == b"IDAT":
            idat += data
        elif typ == b"IEND":
            break
    bpp = {0: 1, 2: 3, 4: 2, 6: 4}[ct]
    stride = w * bpp
    raw = zlib.decompress(idat)
    out, prev, i = bytearray(), bytearray(stride), 0
    for _ in range(h):
        f = raw[i]; i += 1
        line = bytearray(raw[i:i + stride]); i += stride
        if f == 1:
            for x in range(bpp, stride):
                line[x] = (line[x] + line[x - bpp]) & 0xFF
        elif f == 2:
            for x in range(stride):
                line[x] = (line[x] + prev[x]) & 0xFF
        elif f == 3:
            for x in range(stride):
                a = line[x - bpp] if x >= bpp else 0
                line[x] = (line[x] + ((a + prev[x]) >> 1)) & 0xFF
        elif f == 4:
            for x in range(stride):
                a = line[x - bpp] if x >= bpp else 0
                b = prev[x]
                c = prev[x - bpp] if x >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + pr) & 0xFF
        out += line
        prev = line
    return w, h, bpp, out


def variants(r, g, b):
    """Every byte order a 32bpp or 24bpp layer might use, plus 16 bit."""
    v = {
        "BGRA": bytes((b, g, r, 0xFF)),
        "RGBA": bytes((r, g, b, 0xFF)),
        "ARGB": bytes((0xFF, r, g, b)),
        "ABGR": bytes((0xFF, b, g, r)),
        "BGR0": bytes((b, g, r, 0x00)),
        "RGB0": bytes((r, g, b, 0x00)),
        "BGRA-pre": bytes((b * 1, g, r, 0xFF)),
        "BGR24": bytes((b, g, r)),
        "RGB24": bytes((r, g, b)),
        "RGB565le": struct.pack("<H", ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)),
        "RGBA16": struct.pack("<HHHH", r * 257, g * 257, b * 257, 0xFFFF),
        "BGRA16": struct.pack("<HHHH", b * 257, g * 257, r * 257, 0xFFFF),
    }
    return v


def main():
    shot = "/private/tmp/lq_fill_shot.png"
    adb("exec-out", "screencap", "-p", stdout=open(shot, "wb"))
    w, h, bpp, px = read_png(shot)
    # The canvas sits inside the SAI window; read a spot well inside the paper.
    cx, cy = int(w * 0.49), int(h * 0.37)
    off = (cy * w + cx) * bpp
    r, g, b = px[off], px[off + 1], px[off + 2]
    print(f"screen {w}x{h}, sampled ({cx},{cy}) = rgb({r},{g},{b})")
    if r > 245 and g > 245 and b > 245:
        print("still white - did the fill happen?")
        return 1

    vs = variants(r, g, b)
    reps = 64        # 64 identical pixels: long enough to be unique, short
                     # enough to survive a tile boundary
    results = []
    for name, unit in vs.items():
        pat = unit * reps
        if len(pat) > 250:
            pat = pat[:250]
        open("/private/tmp/lq_pat.bin", "wb").write(pat)
        shell(f"rm -f {P}/wt_memscan.log {P}/wt_pattern.bin")
        adb("shell", f"run-as lator.forsai sh -c 'cat > {P}/wt_pattern.bin'",
            stdin=open("/private/tmp/lq_pat.bin", "rb"))
        shell(f"touch {P}/wt_memscan")
        subprocess.run(["sleep", "25"], check=True)
        log = adb("exec-out", "run-as lator.forsai sh -c", f"cat {P}/wt_memscan.log",
                  capture_output=True).stdout.decode("utf-8", "replace")
        hits = [l for l in log.splitlines() if l.startswith("HIT")]
        done = any("done:" in l for l in log.splitlines())
        print(f"{name:9s} ({len(unit)}B/px)  hits={len(hits)}  finished={done}")
        for l in hits[:6]:
            print("   ", l)
        results.append((name, len(hits), hits))
    shell(f"rm -f {P}/wt_pattern.bin")
    return 0


if __name__ == "__main__":
    sys.exit(main())
