#!/usr/bin/env python3
"""
读 wintab32.dll 写的状态文件，给出结论。

DLL 在 SAI 进程里每秒写一次 `C:\\wt_status.txt`（wtc_format_status / wtc_format_screens），
内容形如：

    v=1
    build=Nov 14 2026 21:03:11
    open=1
    recv=1834
    posted=1790
    fetched=1788
    pmax=4095
    newscreen=0
    mismatch=0

在安卓上取它：

    adb shell cat /data/data/com.winlator/files/.../drive_c/wt_status.txt
    # 或在容器里用记事本/终端打开 C:\\wt_status.txt

本脚本把四步链路（加载 → 开上下文 → 收包 → SAI 取包）翻成人话，
省得对着数字猜。也支持 `--liveness 2` 采两次做差值——因为 recv 是累计值，
单看一次只能知道"有没有过流量"，两次才知道"现在还在不在流"。
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path


def parse(text: str) -> dict[str, str]:
    out: dict[str, str] = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or "=" not in line:
            continue
        k, _, v = line.partition("=")
        out[k.strip()] = v.strip()
    return out


def num(d: dict[str, str], k: str) -> int | None:
    try:
        return int(d.get(k, ""))
    except ValueError:
        return None


def report(d: dict[str, str], prev: dict[str, str] | None = None) -> int:
    if not d:
        print("文件为空或不存在。")
        print("  → DLL 没被加载，或 SAI 还没启动。先看 wtlog.txt（WT_DEBUG=1）有没有")
        print("    '==== OwnTab wintab32.dll loaded; built ...' 这一行。")
        return 2

    build = d.get("build", "?")
    open_ = num(d, "open")
    recv = num(d, "recv")
    posted = num(d, "posted")
    fetched = num(d, "fetched")
    pmax = num(d, "pmax")
    newscreen = num(d, "newscreen") or 0
    mismatch = num(d, "mismatch") or 0

    print(f"build   : {build}")
    print(f"pmax    : {pmax}")
    print(f"open    : {open_}      （1 = SAI 打开了 WinTab 上下文）")
    print(f"recv    : {recv}      （DLL 收到的 UDP 笔样本数）")
    print(f"posted  : {posted}      （往 SAI 投的 WT_PACKET 数）")
    print(f"fetched : {fetched}      （SAI 真正取走的包数）")

    if prev:
        pr, pp, pf = num(prev, "recv"), num(prev, "posted"), num(prev, "fetched")
        if None not in (pr, recv, pp, posted, pf, fetched):
            print()
            print(f"Δ2s     : recv+{recv - pr}  posted+{posted - pp}  fetched+{fetched - pf}")

    print()
    print("链路判定：")

    if open_ != 1:
        print("  [X] 加载了，但 SAI 没开 WinTab 上下文")
        print("      → SAI 里打开「选项 → 数位板 → Use WinTab API」，然后重启 SAI")
        print("      → 若已打开，看 wtlog.txt 里的 'WTOpenW hwnd=...' 是否出现")
        return 3
    print("  [OK] 加载 + 上下文已开")

    v = recv if prev is None else (recv - (num(prev, "recv") or 0))
    if not v:
        print("  [X] 没有笔样本到达")
        print("      → 发送端没跑：Winlator 补丁未生效，或端口/回环不通")
        print("      → 用一个已知能发的替代先排除端口问题：")
        print("        python tools/sai_pen_test.py --count 30   （从 PC 上发，仅验证端口）")
        return 4
    print(f"  [OK] 有笔样本到达（recv={recv}）")

    if not posted:
        print("  [X] 收到样本但没投给 SAI")
        print("      → 通常是小节笔在悬停：DLL 只在 press>0 或带位置时才发 WT_PACKET")
        print("      → 按住笔画一笔再看")
        return 5
    print("  [OK] 已投递 WT_PACKET")

    if not fetched:
        print("  [X] 投了但 SAI 没取（消息进队列了却没人读）")
        print("      → 检查 SAI 是否卡住；看 wtlog.txt 的 'WTPacket #' 行")
        return 6
    print("  [OK] SAI 在取包 —— 压感链路是通的")

    print()
    if newscreen:
        print("  [!] newscreen=1：笔落到了 SAI 上下文没覆盖的屏幕上（接了新显示器？）")
        print("      → 该屏上笔当鼠标用，重启 SAI 才有压感")
    if mismatch:
        print("  [!] mismatch=1：DLL 自检发现笔位置和 Wine 光标持续不符，已改为跟随光标")
        print("      → 通常是容器分辨率与补丁传给 configure() 的不一致")

    if posted and fetched and posted - fetched > 40:
        print(f"  [!] 积压 {posted - fetched} 包：SAI 消费跟不上（画布太大/CPU 弱），会感觉笔拖尾")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="读 wt_status.txt 给结论")
    ap.add_argument("path", nargs="?", help="wt_status.txt 路径；省略则读 C:\\wt_status.txt")
    ap.add_argument("--liveness", type=float, default=0, metavar="秒",
                    help="采两次做差值（默认 0 = 只读一次）")
    args = ap.parse_args()

    p = Path(args.path) if args.path else Path(r"C:\wt_status.txt")
    try:
        text = p.read_text(encoding="utf-8", errors="replace")
    except OSError as e:
        print(f"读不到 {p}: {e}")
        return 2

    d = parse(text)
    if not args.liveness:
        return report(d)

    print("第一次：")
    report(d)
    time.sleep(args.liveness)
    print()
    print(f"--- {args.liveness:g} 秒后 ---")
    try:
        d2 = parse(p.read_text(encoding="utf-8", errors="replace"))
    except OSError as e:
        print(f"读不到 {p}: {e}")
        return 2
    return report(d2, prev=d)


if __name__ == "__main__":
    sys.exit(main())
