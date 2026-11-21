# 通用 WinTab 压感（0.3.11）

> 目标：不再只适配 SAI 的 WinTab 用法。FireAlpaca 等应用虽然也用 WinTab，
> 但读取报文的方式和字段布局与 SAI 不同，必须按应用请求的 `lcPktData` 构造报文。

## 两个关键差异

### 1. 报文布局不同

`WTOpenW` 的 `lcPktData` 决定每条 packet 的字段顺序和长度：

- SAI：`0x15e2`，36 字节，状态/光标/按键/X/Y/压力/方向；
- FireAlpaca：`0x1fe0`，40 字节，光标/按键/X/Y/Z/法向压力/切向压力/方向。

旧 DLL 一律强制成 SAI 的 `0x15e2`，FireAlpaca 按自己的 `0x1fe0` 读，
压力字段错位，所以没有压感。

现在 `wt_build_packet()` 按应用请求的 mask 逐字段写入；`WTPacket()` 和
`WTPacketsGet()` 都走同一套布局。

### 2. 取包方式不同

- SAI：处理 `WT_PACKET` 消息，再调 `WTPacket(serial)`；
- FireAlpaca：不处理消息，直接轮询 `WTPacketsGet(max)`，一次取一批。

旧 DLL 的 `WTPacketsGet()` 只返回最后一包，而且和 UDP 线程抢读 socket；
FireAlpaca 常常拿到 0 包或压力为 0。

现在：

- `WTPacketsGet()` 从 ring 里按序返回最多 `max` 包，推进 `g_deliver_serial`；
- `g_polling=1` 时生产者不做 SAI 的点击 hold，也不做消息 conflation，
  每条样本都进 ring；
- 应用请求的 mask 不是 `0x15e2` 时自动进入 polling 模式；
- 32 位应用走 `WTOpenA`，同样记录请求 mask。

## 32 位 + 64 位

`PenBridge` 现在内嵌两份 DLL：

- `system32/wintab32.dll`：x86_64，91,648 字节；
- `syswow64/wintab32.dll`：i686，84,480 字节。

两者由同一份 `bridge/wintab32.c` 编译，启动时按大小自愈。Wine 的
`wintab32=native,builtin` 同时覆盖 32/64 位。

## 构建

`bridge/timelapse_win.h` 是公开树里缺失的录像模块的空实现（不影响压感）。
用 LLVM-mingw：

```bash
x86_64-w64-mingw32-gcc -shared -O2 -o wintab32.dll wintab32.c wintab32.def \
  -lgdi32 -luser32 -lws2_32 -municode
i686-w64-mingw32-gcc -shared -O2 -o wintab32-32.dll wintab32.c wintab32.def \
  -lgdi32 -luser32 -lws2_32 -municode
```

## 验证状态

- FireAlpaca 64 位：请求 `0x1fe0`，`WTPacketsGet` 返回 `size=40`，`fetched` 持续增长；
- Krita 5.3.4.1（64 位）：请求 `0x1fe4`（44 字节），`WTPacketsGet` 返回 `size=44`；
  Krita 数位板测试器仍显示 Mouse 事件，压感字段未生效，**待继续排查**；
- 注意 Krita 测试器字段含义：`B`=buttons，`P`=pressure，`S`=speed；
  之前把 `S` 当成压力是误读；
- SAI：仍使用 `0x15e2`/`WTPacket` 路径；
- 32 位 DLL 已内嵌到 `syswow64`，同一份源码编译，等待 32 位应用实测。
