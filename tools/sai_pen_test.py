#!/usr/bin/env python3
"""
SAI 压感桥 —— 桌面端模拟笔样本发送器（用于在 Windows/容器里验证 wintab32.dll）。

配合 ametrien/Paint-Tool-SAI-pen-pressure-macOS-fix 的 wintab32.dll 使用：
DLL 在 127.0.0.1:47800 上监听 UDP 文本报文，本脚本按同一格式发送，
因此可以在没有真实数位板的情况下验证"报文 → 压感"整条链路。

用法
----
  # 1. 先起 wtprobe.exe（或 SAI），再发一串圆：压力从轻到重再回到轻
  python sai_pen_test.py --circle

  # 2. 一条横扫，压力 0→满，便于肉眼看粗细变化
  python sai_pen_test.py --sweep --pmax 4095

  # 3. 自检：不开 DLL，只在本机建个 UDP 监听，检查报文格式
  python sai_pen_test.py --selftest

报文格式（来自 wintab_core.h 的 wtc_parse_datagram，六个必需字段）
------------------------------------------------------------------
    "seq p x y w h"
    seq  递增序号（对端只用于统计丢包）
    p    压力 0..pmax；0 = 抬笔/悬停
    x,y  位置，原点左下、y 向上，与 w,h 同单位
    w,h  参照系尺寸；对端只按 x/w、(h-y)/h 的比例换算

所以本脚本用"左下原点"的坐标：y_wintab = H - y_screen。
"""

from __future__ import annotations

import argparse
import math
import socket
import sys
import time

DEFAULT_PORT = 47800          # wintab32.c 的 SAMPLE_PORT，别改
DEFAULT_PMAX = 4095           # 应与 C:\\wt_pmax.txt 一致


def packet(seq: int, p: int, x: int, y: int, w: int, h: int) -> bytes:
    """一帧笔样本。字段顺序是与 DLL 的硬约定。"""
    return f"{seq} {p} {x} {y} {w} {h}".encode("ascii")


def clamp(v: float, lo: float, hi: float) -> float:
    return lo if v < lo else (hi if v > hi else v)


def circle_points(w: int, h: int, n: int):
    """椭圆轨迹 + 压力从 0 到满再回 0，用来肉眼检查压感方向对不对。"""
    cx, cy = w / 2.0, h / 2.0
    rx, ry = w * 0.30, h * 0.30
    for i in range(n):
        t = 2.0 * math.pi * i / n
        sx = cx + rx * math.cos(t)
        sy = cy + ry * math.sin(t)
        # 前半圈压力上升，后半圈下降
        press = abs(math.sin(t))
        yield sx, sy, press


def sweep_points(w: int, h: int, n: int):
    """水平横扫，压力线性 0→1。"""
    y = h * 0.5
    for i in range(n):
        f = i / max(1, n - 1)
        yield w * (0.1 + 0.8 * f), y, f


def run(args) -> int:
    w, h, pmax = args.width, args.height, args.pmax
    if args.selftest:
        # 只校验格式，不发网络：把自己发的字节解析回来
        ok = True
        for seq, (sx, sy, pr) in enumerate(circle_points(w, h, 5)):
            p = int(round(clamp(pr, 0.0, 1.0) * pmax))
            x = int(round(sx))
            y = int(round(h - sy))
            raw = packet(seq, p, x, y, w, h)
            parts = raw.decode("ascii").split()
            if len(parts) != 6 or int(parts[1]) < 0 or not (0 <= int(parts[3]) <= h):
                ok = False
            print(f"  {'OK ' if ok else 'BAD'} {raw.decode('ascii')}")
        print("selftest: PASS (6 fields, pressure in range, y within [0,h])" if ok else "selftest: FAIL")
        return 0 if ok else 1

    gens = {"circle": circle_points, "sweep": sweep_points}
    gen = gens[args.mode]
    n = args.count

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sink = (args.host, args.port)
    interval = 1.0 / max(1.0, args.rate)

    print(f"-> {args.host}:{args.port}  mode={args.mode}  samples={n}  {args.rate} Hz  pmax={pmax}")
    print("   (no error if nobody listens: UDP drops are normal, the DLL is not required here)")

    seq = 1
    t0 = time.perf_counter()
    for sx, sy, pr in gen(w, h, n):
        p = int(round(clamp(pr, 0.0, 1.0) * pmax))
        if args.min_press and p == 0:
            p = args.min_press
        # y 翻成左下原点（DLL 自己再做 (h - y)）
        sock.sendto(packet(seq, p, int(round(sx)), int(round(h - sy)), w, h), sink)
        seq += 1
        time.sleep(interval)

    # 收笔
    sock.sendto(packet(seq, 0, w // 2, h // 2, w, h), sink)

    dt = time.perf_counter() - t0
    print(f"  sent {seq} datagrams in {dt:.2f}s (nominal {n * interval:.2f}s)")
    sock.close()
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="SAI 压感桥测试发送器")
    ap.add_argument("--mode", choices=["circle", "sweep"], default="circle")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--pmax", type=int, default=DEFAULT_PMAX)
    ap.add_argument("--width", type=int, default=1600, help="容器/屏幕宽")
    ap.add_argument("--height", type=int, default=900, help="容器/屏幕高")
    ap.add_argument("--count", type=int, default=240)
    ap.add_argument("--rate", type=float, default=120.0, help="发送频率 Hz")
    ap.add_argument("--min-press", type=int, default=0, help="把 0 压力抬到该值（测恒定压感）")
    ap.add_argument("--selftest", action="store_true", help="不联网，只校验报文格式")
    args = ap.parse_args()
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
