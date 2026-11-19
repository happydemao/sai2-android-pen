#!/usr/bin/env python3
"""
wintab32.dll 的"纯逻辑"参考实现（Python 转写）。

来源：ametrien/Paint-Tool-SAI-pen-pressure-macOS-fix 的
      wintab-src/wintab_core.h（MIT）
      - wtc_parse_datagram()   报文解析
      - wtc_parse_control()    控制行解析
      - wtc_parse_sample()     文件回退格式解析
      - wtc_place()            笔位置 → SAI 平板区域内的坐标
      - wtc_map_to_out()       再映射到 SAI 打开的输出坐标系

为什么要有这个文件：那个 DLL 我下载不到（沙箱的 TLS 被拦），但它的纯逻辑是
确定性的、可翻译的。有了参考实现，就能在没有 DLL、没有数位板、没有 SAI 的情况下，
验证"我们发出去的字节，被解析后落到的位置是不是我们想要的位置"——
尤其是 y 轴翻转错误（笔画上下镜像）这种靠肉眼很难立刻发现、但一画就废的问题。

注意：这不是给生产用的库，是测试用的对照物。逐行对应 C，不追求 Pythonic。
"""

from __future__ import annotations

from dataclasses import dataclass, field

WTC_MAX_PRESS = 8191       # 解析器能接受的上限（实际 pmax 由 C:\wt_pmax.txt 决定）

PK_STATUS = 0x0002
PK_CURSOR = 0x0020
PK_BUTTONS = 0x0040
PK_X = 0x0080
PK_Y = 0x0100
PK_NORMAL_PRESSURE = 0x0400
PK_ORIENTATION = 0x1000
OUR_PKTDATA = (PK_STATUS | PK_CURSOR | PK_BUTTONS | PK_X | PK_Y
               | PK_NORMAL_PRESSURE | PK_ORIENTATION)   # 0x15e2

IN_EXT = 32767


def _tdiv(a: int, b: int) -> int:
    """C 的整数除法：向零截断。Python 的 // 是向下取整，负数会不同。"""
    if b == 0:
        return 0
    q = abs(a) // abs(b)
    return q if (a < 0) == (b < 0) else -q


# ---------------------------------------------------------------------------
# 报文解析
# ---------------------------------------------------------------------------

@dataclass
class Times:
    has: bool = False
    ev: int = 0
    tx: int = 0
    n: int = 0
    ev_bad: bool = False


@dataclass
class Dgram:
    seq: int = -1
    p: int = -1
    x: int = 0
    y: int = 0
    w: int = 0
    h: int = 0
    tm: Times = field(default_factory=Times)


def parse_datagram(buf: str) -> Dgram | None:
    """对应 wtc_parse_datagram()。返回 None = 不是笔样本。

    C 里是 sscanf("%ld %d %d %d %d %d %u %u %u %d")，所以：
      - 至少 6 个字段解析成功，且 p >= 0，才算样本
      - 第 7/8/9/10 个字段（ev tx n evok）可缺省
      - 样本必须以数字开头；控制行以字母开头，会在这里被拒
    """
    d = Dgram()
    toks = buf.split()
    if len(toks) < 6:
        return None
    vals: list[int] = []
    for i, t in enumerate(toks[:10]):
        try:
            vals.append(int(t))
        except ValueError:
            break
    if len(vals) < 6:
        return None
    d.seq, d.p, d.x, d.y, d.w, d.h = vals[0], vals[1], vals[2], vals[3], vals[4], vals[5]
    if d.p < 0:
        return None
    if len(vals) >= 9:
        d.tm = Times(has=True, ev=vals[6] & 0xFFFFFFFF,
                     tx=vals[7] & 0xFFFFFFFF, n=vals[8] & 0xFFFFFFFF,
                     ev_bad=(len(vals) >= 10 and vals[9] != 1))
    return d


CTL_ZOOM = 1
CTL_WAKE = 2
ZOOM_MAX_STEPS = 64


@dataclass
class Control:
    kind: int = 0
    zoom: int = 0
    wake: str = ""


def parse_control(buf: str) -> Control | None:
    """对应 wtc_parse_control()。首字符必须是 Z 或 K，第二个字符必须是空格。"""
    if len(buf) < 2 or buf[1] != " ":
        return None
    c = Control()
    if buf[0] == "Z":
        p = buf[2:].strip()
        try:
            v = int(p)
        except ValueError:
            return None
        if v == 0 or abs(v) > ZOOM_MAX_STEPS:
            return None
        c.kind, c.zoom = CTL_ZOOM, v
        return c
    if buf[0] == "K":
        c.kind, c.wake = CTL_WAKE, buf[2:].strip()
        return c
    return None


@dataclass
class Sample:
    press: int = 0
    x: int = 0
    y: int = 0
    w: int = 0
    h: int = 0
    has_pos: bool = False


def parse_sample(buf: str, out: Sample | None = None) -> Sample | None:
    """对应 wtc_parse_sample()：文件回退格式 "p [x y w h]"。"""
    s = out if out is not None else Sample()
    toks = buf.split()
    if not toks:
        return None
    try:
        p = int(toks[0])
    except ValueError:
        return None
    if p < 0:
        return None
    s.press = min(p, WTC_MAX_PRESS)
    if len(toks) >= 5:
        try:
            x, y, w, h = (int(t) for t in toks[1:5])
        except ValueError:
            s.has_pos = False
            return s
        if w > 0 and h > 0:
            s.x, s.y, s.w, s.h, s.has_pos = x, y, w, h, True
            return s
    s.has_pos = False
    return s


# ---------------------------------------------------------------------------
# 坐标映射
# ---------------------------------------------------------------------------

@dataclass
class Rect:
    x: int
    y: int
    w: int
    h: int


def map_to_out(s: Sample, oX, oY, eX, eY, in_ext):
    """对应 wtc_map_to_out()：把样本位置映射到 SAI 的 lcOut 坐标系（y 向上）。"""
    if s.w <= 0 or s.h <= 0:
        return oX, oY
    if not eX:
        eX = in_ext
    if not eY:
        eY = in_ext
    out_x = oX + _tdiv(s.x * eX, s.w)
    if eY > 0:
        out_y = oY + _tdiv(s.y * eY, s.h)
    else:
        out_y = oY + _tdiv((s.h - 1 - s.y) * (-eY), s.h)
    return out_x, out_y


def place(s: Sample, virt: Rect, sys: Rect, oX, oY, eX, eY, in_ext):
    """对应 wtc_place()。返回 (outx, outy, inside)。"""
    if s.w <= 0 or s.h <= 0 or virt.w <= 0 or virt.h <= 0 or sys.w <= 0 or sys.h <= 0:
        x, y = map_to_out(s, oX, oY, eX, eY, in_ext)
        return x, y, True

    wx8 = virt.x * 8 + _tdiv(s.x * virt.w * 8, s.w)
    wy8 = virt.y * 8 + _tdiv((s.h - s.y) * virt.h * 8, s.h)

    sw8 = sys.w * 8
    sh8 = sys.h * 8
    lx = wx8 - sys.x * 8
    ly = (sys.y + sys.h) * 8 - wy8

    inside = 0 <= lx <= sw8 and 0 <= ly <= sh8
    if not inside:
        lx = max(0, min(lx, sw8 - 1))
        ly = max(0, min(ly, sh8))
    local = Sample(press=s.press, x=lx, y=ly, w=sw8, h=sh8, has_pos=True)
    ox, oy = map_to_out(local, oX, oY, eX, eY, in_ext)
    return ox, oy, inside


# ---------------------------------------------------------------------------
# 默认上下文的输出范围（fill_default_context 里的那组值）
# ---------------------------------------------------------------------------

def default_context_out(virt_w: int, virt_h: int):
    """fill_default_context()：lcOutExt = 虚拟桌面尺寸 × 8，lcSysOrg=(0,0)。"""
    return dict(oX=0, oY=0, eX=virt_w * 8, eY=virt_h * 8, in_ext=IN_EXT,
                sys=Rect(0, 0, virt_w, virt_h))
