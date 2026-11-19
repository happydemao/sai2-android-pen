#!/usr/bin/env python3
"""
端到端校验：SaiPressureBridge（安卓发送端）→ UDP 报文 → wintab32.dll 的解析/映射逻辑。

不需要安卓设备、不需要 SAI、不需要数位板，也不需要那个 DLL 二进制：
  · 发送端用真实的 patch/SaiPressureBridge.java（javac 现编）
  · 接收侧用 tools/wintab_reference.py（wintab_core.h 的逐行转写）

它断言的是四件事：
  1. 每个报文都能被解析器接受（字段数/取值范围），不会被误判成控制行
  2. 位置编码是 "y 向上"（发错方向就整幅画上下镜像，肉眼要画一会儿才发现）
  3. SAI 侧最终拿到的坐标与笔尖在屏幕上的位置成同一比例
  4. 压力按 pmax 线性缩放，且 0 只出现在抬笔

用法：
    python tools/verify_bridge.py            # 默认 1600x900
    python tools/verify_bridge.py --w 1920 --h 1200
    python tools/verify_bridge.py --keep     # 保留临时编译目录，便于排查
"""

from __future__ import annotations

import argparse
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))

import wintab_reference as ref  # noqa: E402

PORT = 47800
PMAX = 4095


class Receiver(threading.Thread):
    def __init__(self):
        super().__init__(daemon=True)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", PORT))
        self.sock.settimeout(0.3)
        self.packets: list[str] = []
        self.stopped = False

    def run(self):
        self.sock.settimeout(0.3)
        while not self.stopped:
            try:
                data, _ = self.sock.recvfrom(256)
            except socket.timeout:
                continue
            except OSError:
                return
            self.packets.append(data.decode("ascii", "replace"))

    def stop(self):
        self.stopped = True
        time.sleep(0.35)
        self.sock.close()


def _build_root() -> Path:
    """编译目录放在工作区内：系统临时目录在受限环境里可能不可写。"""
    d = ROOT / ".build"
    d.mkdir(parents=True, exist_ok=True)
    return d


def build(tmp: Path) -> Path:
    javac = shutil.which("javac")
    if not javac:
        sys.exit("找不到 javac：请先装 JDK（本机已装 java 25，但没在 PATH 里？）")
    classes = tmp / "classes"
    classes.mkdir(parents=True, exist_ok=True)
    srcs = [ROOT / "patch" / "SaiPressureBridge.java", HERE / "java" / "ProbeSender.java"]
    r = subprocess.run([javac, "-encoding", "UTF-8", "-d", str(classes)] + [str(s) for s in srcs],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout)
        print(r.stderr)
        sys.exit("javac 失败")
    return classes


def run_sender(classes: Path, w: int, h: int) -> list[tuple[float, float, float, bool]]:
    java = shutil.which("java") or "java"
    r = subprocess.run([java, "-cp", str(classes), "ProbeSender", str(w), str(h)],
                       capture_output=True, text=True, timeout=60)
    if r.returncode != 0:
        print(r.stdout)
        print(r.stderr)
        sys.exit(f"ProbeSender 退出码 {r.returncode}")
    out = []
    for line in r.stdout.splitlines():
        f = line.split()
        if len(f) == 4 and f[0] == "S":
            x, y, p = float(f[1]), float(f[2]), float(f[3])
            out.append((x, y, p, p > 0))
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--w", type=int, default=1600)
    ap.add_argument("--h", type=int, default=900)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()
    w, h = args.w, args.h

    # 用固定目录而不是 mkdtemp：受限环境下 mkdtemp 出来的目录不允许再建子目录
    tmp = _build_root() / "verify"
    shutil.rmtree(tmp, ignore_errors=True)
    tmp.mkdir(parents=True, exist_ok=True)
    errors: list[str] = []
    try:
        classes = build(tmp)

        rx = Receiver()
        rx.start()
        time.sleep(0.15)
        sent = run_sender(classes, w, h)
        time.sleep(0.4)
        rx.stop()

        got = rx.packets
        print(f"发送 {len(sent)} 个样本，收到 {len(got)} 个报文  (容器 {w}x{h})")
        if not got:
            print("没有任何报文到达 —— 检查 47800 端口是否被占用、或桥没启动")
            return 1
        if len(got) != len(sent):
            print(f"  注意：数量不一致（合并/丢包都会这样），按前 {min(len(got), len(sent))} 条比对")

        ctx = ref.default_context_out(w, h)
        virt = ref.Rect(0, 0, w, h)

        print()
        print(f"{'screen(x,y)':>16} {'wire(x,y)':>14} {'outx/e':>8} {'outy/e':>8} {'exp_x':>8} {'exp_y':>8}  p    判定")
        print("-" * 100)

        n = min(len(got), len(sent))
        for i in range(n):
            sx, sy, sp, down = sent[i]
            raw = got[i].strip()
            before = len(errors)
            d = ref.parse_datagram(raw)
            if d is None:
                errors.append(f"[{i}] 解析失败: {raw!r}")
                continue
            if ref.parse_control(raw) is not None:
                errors.append(f"[{i}] 被误判成控制行: {raw!r}")
                continue

            # 1. 压力。容差 ±1：Java 用 float 算 ((float)p * 4095) 再 Math.round，
            #    Python 用 double；0.7f 是 0.69999998，两者可能差 1 步。
            #    这不是 bug —— 但它提醒：pmax 必须两端一致，否则整段压力会被截断。
            exp_p = round(min(max(sp, 0.0), 1.0) * PMAX) if down else 0
            if down and exp_p < 1:
                exp_p = 1
            if abs(d.p - exp_p) > 1:
                errors.append(f"[{i}] 压力 {d.p} 与期望 {exp_p} 相差超过 1")

            # 2. y 向上
            exp_wire_y = round(h - sy)
            if d.y != exp_wire_y:
                errors.append(f"[{i}] wire y {d.y} != 期望 {exp_wire_y}（y 轴方向错了）")

            # 3. 比例映射
            s = ref.Sample(press=d.p, x=d.x, y=d.y, w=d.w, h=d.h, has_pos=True)
            ox, oy, inside = ref.place(s, virt, ctx["sys"], ctx["oX"], ctx["oY"],
                                       ctx["eX"], ctx["eY"], ctx["in_ext"])
            fx = ox / ctx["eX"]
            fy = oy / ctx["eY"]
            exp_x = sx / w
            exp_y = (h - sy) / h
            if not inside:
                errors.append(f"[{i}] 被判为越界（outside）")
            if abs(fx - exp_x) > 0.002:
                errors.append(f"[{i}] x 比例 {fx:.4f} != {exp_x:.4f}")
            if abs(fy - exp_y) > 0.002:
                errors.append(f"[{i}] y 比例 {fy:.4f} != {exp_y:.4f}")

            bad = len(errors) > before
            print(f"{sx:7.1f},{sy:6.1f} {d.x:6d},{d.y:6d} {fx:8.4f} {fy:8.4f} "
                  f"{exp_x:8.4f} {exp_y:8.4f}  {d.p:4d}  {'FAIL' if bad else 'OK'}")

        # 4. 控制报文
        ctl = ref.parse_control("Z 3")
        if ctl is None or ctl.kind != ref.CTL_ZOOM or ctl.zoom != 3:
            errors.append("控制行 'Z 3' 解析失败")
        if ref.parse_control("Z 0") is not None:
            errors.append("'Z 0' 应被拒绝（0 步无意义）")
        if ref.parse_control("Z 65") is not None:
            errors.append("'Z 65' 应被拒绝（超过 64）")
        if ref.parse_datagram("Z 3") is not None:
            errors.append("控制行被当成笔样本")
        if ref.parse_control("1 2 3 4 5 6") is not None:
            errors.append("笔样本被当成控制行")

        print()
        if errors:
            print(f"[FAIL] {len(errors)} issue(s):")
            for e in errors[:20]:
                print("   " + e)
            return 1
        print("[PASS] parse + y-up + ratio mapping + pressure scaling + control/sample separation")
        return 0
    finally:
        if args.keep:
            print(f"\n临时目录保留：{tmp}")
        else:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
