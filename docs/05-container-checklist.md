# 容器内验证检查单（设备到手后按顺序跑）

这条链路有四个断点，每个断点都有**源码里真实存在的日志行或状态字段**作为判据。
不要靠"画一笔看有没有压感"来判断——那样一次只能告诉你"坏了"，
而下面每一步都能告诉你是**哪一段**坏了。

```
① DLL 被 SAI 加载 ──► ② SAI 打开 WinTab 上下文 ──► ③ 收到 UDP 样本 ──► ④ SAI 取走包
   (wtlog 第一行)        (open=1)                    (recv 在涨)          (fetched 在涨)
```

---

## 阶段 0：PC 上已完成（不需要设备）

| 项 | 结果 |
|---|---|
| 报文格式 vs DLL 解析器 | ✅ `tools/verify_bridge.py`：30/30 通过（解析、y 向上、比例映射、压力、控制行隔离） |
| 发送端 Java 代码 | ✅ javac 编译通过，端到端跑通 |
| 状态文件判读 | ✅ `tools/read_status.py` 对三种故障态给出正确结论与退出码 |

剩下唯一没验的，就是**它能不能在 Box64 里加载**——这必须有设备。

---

## 阶段 1：把东西推进去

```bash
# 设备连上后
adb devices -l
adb shell getprop ro.product.cpu.abi           # 应含 arm64
adb shell pm list packages | findstr winlator   # 确认包名，常见 com.winlator

# 推送素材（先落到 sdcard，再由容器/D 盘映射进去）
adb push wintab32.dll        /sdcard/Download/
adb push wtprobe.exe         /sdcard/Download/     # 阶段 2 用得到
adb push wt_pmax.txt         /sdcard/Download/     # 内容就一行：4095
```

容器目录名因版本/分支而异，先找出来：

```bash
adb shell run-as com.winlator ls files            # 仅 debug 包可 run-as
# 或（有 root）
adb shell su -c "find /data/data/com.winlator -name system32 -maxdepth 8 -type d"
```

---

## 阶段 2：先只验"Box64 能不能加载这个 DLL"（不碰 SAI、不碰压感）

**这是整个移植最大的未知数**，用 `wtprobe.exe` 单独验，不需要 SAI：

```bash
# 容器里：把 wtprobe.exe + wintab32.dll 放到同一个目录，双击跑
# 然后开 WT_DEBUG=1（容器环境变量），看 C:\wtlog.txt
```

**通过判据**：`wtlog.txt` 里出现这一行（源码里的原串）：

```
==== OwnTab wintab32.dll loaded; built <日期> <时间>; screen 1600x900 virtual 1600x900 at (0,0) maxPress=4095 host=Test pen probe (no wake) ====
```

- `host=Test pen probe (no wake)` 说明进程名被识别成 `wtprobe.exe` —— 也顺带证明 DLL 的
  `GetModuleFileNameA` 在这个环境里工作正常。
- 出现 `producer: listening on UDP 127.0.0.1:47800 (blocking, immediate post)` 说明
  **winsock 在 Box64 + Wine 下能正常 bind**。
- 出现 `maxPress=4095` 说明 `C:\wt_pmax.txt` 被正确读到（这决定 SAI 看到的压力轴上限）。

**没出现** → 不要往下走。日志空白通常是 DLL 根本没被加载（目录不对/位数不对/
`WINEDLLOVERRIDES` 没生效）。

---

## 阶段 3：装进 SAI 的容器

1. `wintab32.dll` → `drive_c/windows/system32/wintab32.dll`
2. 容器环境变量：`WINEDLLOVERRIDES=wintab32=n,b`
3. `drive_c/wt_pmax.txt` → `4095`（**DLL 只在加载时读一次**，改完要重启容器）
4. `WT_DEBUG=1`（先开着，通了再关）
5. 启动 SAI（默认即走 WinTab；没压感时再查 **选项 → 数位板** 的 API 选项）

看状态文件：

```bash
adb shell cat "<容器>/drive_c/wt_status.txt"
python tools/read_status.py wt_status.txt          # 本地分析；或直接看
```

`open=1` = SAI 真的开了 WinTab 上下文。此时 SAI 已经"看见笔了"，只是还没人喂数据。

---

## 阶段 4：让样本进来

两个选择，**先做简单的那个**：

**(a) 不装补丁，先证明"端口 + 报文 + 解析"在容器里通**
从 PC 直接往设备的 47800 发（adb 端口转发，或同一 Wi-Fi）：

```bash
adb forward tcp:47800 udp:47800     # 注意：adb forward 不支持 udp
```
⚠️ `adb forward` 只支持 TCP。所以这一步要么用同一局域网、要么用 `adb shell` 里的
toybox 工具，实际最省事的是**跳过 (a)，直接 (b)**，用阶段 2 的 `wtprobe` 已经证明了端口可用。

**(b) 装补丁，真机发**
按 [README.md](./01-feasibility.md) 改 `TouchpadView.java`，编译，安装，容器设 `Touchscreen` 输入模式。

**判据**（两次采样，`read_status.py --liveness 2` 会自己做差值）：

| 字段 | 含义 | 期望 |
|---|---|---|
| `recv` | DLL 收到的 UDP 样本数 | 画一笔时在涨 |
| `posted` | 投给 SAI 的 WT_PACKET 数 | 跟着涨 |
| `fetched` | SAI 取走的包数 | 跟着涨 |
| `posted - fetched` | 积压 | 小于 ~40，否则 SAI 消费跟不上（拖尾） |

日志里还会持续出现（源码原串）：

```
RATE recv=<n> fetches=<n> gap=<n> tick=<ms>
WTPacket #1 serial=1 x=... y=... press=... lat=...ms
```

`gap` 是序号缺口 = 真实丢包数。`gap` 一直涨而 `recv` 不涨 → 发送端在发但被丢，
优先怀疑发送频率过高或回环被限流。

---

## 阶段 5：压感真的生效了吗

链路通了 ≠ 笔画有粗细。最后一公里在 SAI 自己身上：

1. 笔刷的 **最小尺寸（Min Size）** 必须是 100% 以下，否则压力怎么变都是满粗。
2. 笔刷的 **笔压勾选** 是否勾上（大小/浓度/不透明度）。
3. 三层曲线相乘：笔硬件曲线 × 我们的映射 × SAI 笔刷设置。**一次只调一层。**

---

## 常见现象对照表

| 现象 | 最可能的原因 | 查什么 |
|---|---|---|
| `wt_status.txt` 不存在 | DLL 没加载 | `wtlog.txt` 第一行；system32 路径；`n,b` 覆盖 |
| `open=0` | SAI 没开 WinTab | SAI 选项；`wtlog` 里有没有 `WTOpenW` |
| `recv` 涨、`posted=0` | 只发了悬停（压力 0） | 补丁里 `down` 判定；`getPressure()` 是否恒 0 |
| `posted` 涨、`fetched=0` | SAI 没在处理消息 | SAI 是否卡死；`WTPacket` 行 |
| 画出来上下镜像 | 坐标 y 方向翻错 | 补丁是否用了 `XForm.transformPoint` 后**又**翻了一次 |
| 笔画整体偏移/只在半屏 | `configure()` 传了手机分辨率而不是容器分辨率 | 改成 `xServer.screenInfo.width/height` |
| 笔迹是折线 | 没取 `getHistorical*()` | 补丁里历史点循环 |
| 压感一直满/一直细 | `pmax` 两端不一致 | `wt_pmax.txt` vs `SaiPressureBridge.PMAX` |

---

## 我会在设备上做的事（需要 adb 时）

1. `adb devices` 确认连接 → `getprop` 取 ABI/Android 版本
2. 定位 Winlator 容器目录，把 DLL / `wt_pmax.txt` 落位，改容器环境变量
3. 装 `wtprobe.exe` 跑阶段 2（**先拿下"Box64 能否加载"这个未知数**）
4. 读 `wtlog.txt` / `wt_status.txt`，把结论告诉你
5. 若加载成功，再进入改 Winlator 源码/编 APK 的环节

第 3 步之前**不需要编译任何东西**，所以设备一连上就有结论。
