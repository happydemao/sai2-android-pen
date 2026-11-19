# 真机实测记录（Redmi K40 Pro / Winlator 11.2 / Box64 0.4.4）

时间：2026-11-17 23:0x–23:2x　｜　设备：Redmi K40 Pro，Android 17 (SDK 37)，arm64-v8a，11.5 GB RAM，KernelSU root

---

## 0. 结论：**最大的未知数已经拿下**

> **那个 macOS 项目的 `wintab32.dll`，在 Box64 + Wine 下能正常加载、能开 WinTab 上下文、能 bind UDP 端口。**
> 之前唯一的"可能全盘皆输"的风险点，现在没有风险了。

探针自己的判定输出（真机原样）：

```
probe=1
dll=loaded
ours=yes
build=Nov 14 2026 17:58:00
entrypoints=ok
info=200
defcontext=ok
ctx=open
msgbase=32752
```

DLL 内部日志（`C:\wtlog.txt`，源码里真实存在的行）：

```
==== OwnTab wintab32.dll loaded; built Nov 14 2026 17:58:00; screen 1280x720 virtual 1280x720 at (0,0) maxPress=4095 host=Test pen probe (no wake) ====
WTInfoW cat=0 idx=0 out=0000000000000000
WTInfoW cat=3 idx=0 out=00000000801EFD80
producer: listening on UDP 127.0.0.1:47800 (blocking, immediate post)
HOLD: ON (first pen-down packets wait for SAI to take the click, at most 30ms)
WTOpenW hwnd=0000000000040034 enable=1
WTOpenW window class=SAIPPProbe: SAI keeps its area until restarted
WTOpenW ctx from SAI: pktData=0x15e2 pktMode=0 moveMask=0x15e2 opts=0x4 msgBase=0x7ff0
                      rate=133 in=(0,0 ext 32767,32767) out=(0,0 ext 10240,5760) sys=(0,0 ext 1280,720) sysMode=0
msg hook: dedup=off scroll-pan=on hold=on
click dedup: hook installed on thread 36
WTClose
```

`C:\wt_status.txt`：

```
v=1
build=Nov 14 2026 17:58:00
open=1
recv=0
posted=0
fetched=0
pmax=4095
newscreen=0
mismatch=0
```

### 逐项对上

| 未知数 | 结论 | 依据 |
|---|---|---|
| Box64 能否执行 UCRT 版 mingw x64 DLL | ✅ 能 | `dll=loaded` + build stamp |
| `api-ms-win-crt-*` 依赖在 Wine 里有吗 | ✅ 有 | 同上，无缺 DLL 报错 |
| `SAIPP_BridgeBuild` 导出能区分自带/自制 | ✅ 能 | `ours=yes` |
| Winsock 能否在 Box64 下 bind 回环 | ✅ 能 | `listening on UDP 127.0.0.1:47800` |
| `C:\wt_pmax.txt` 能否被读到 | ✅ 能 | `maxPress=4095`（没读到时会是 1023） |
| WinTab API 握手是否完整 | ✅ 完整 | `WTInfo=200`、默认上下文、`WTOpenW`、`msgBase=0x7ff0`、`pktData=0x15e2` |
| 消息钩子能否装 | ✅ 能 | `click dedup: hook installed on thread 36` |
| 进程名识别（避免在探针里抢前台） | ✅ 对 | `host=Test pen probe (no wake)` |
| **UDP 样本 → recv → posted → fetched** | ⚠️ **未完成**（测试被中断） | 见下节 |
| 真实 SAI + "Use WinTab API" | ⚠️ 未测 | 需要 GUI 里开设置 |

---

## 1. 环境事实（以后不用再猜）

| 项 | 值 |
|---|---|
| Winlator | **11.2**（versionCode 33, minSdk 26） |
| Box64 | **arm64 v0.4.4**（built 2026-08-03） |
| Wine 前缀 | `/data/data/com.winlator/files/rootfs/home/xuser-1/.wine`（`home/xuser` 是它的符号链接） |
| `drive_c` | 同上 `/drive_c` |
| 容器内 Wine | `rootfs/opt/wine`，**自带 `x86_64-windows/wintab32.dll`**（所以必需 DLL 覆盖，否则加载的是 Wine 自带空实现） |
| 容器默认 Box64 调优 | `BIGBLOCK=3`、`SAFEFLAGS=1`、`STRONGMEM=0`、`FASTNAN=1`、`FASTROUND=1`、`CALLRET=1` |
| 容器 DISPLAY | `:0`，socket 在 `rootfs/tmp/.X11-unix/X0` |
| 容器 `WINEPREFIX` 属主 | uid `10310`（u0_a310） |

> 注意：容器**不用 chroot**，`wine` 进程的 `/` 仍是 Android 根，所有路径都是绝对路径翻译。
> 所以 `WINEPREFIX` 必须用 `/data/user/0/...` 或 `/data/data/...` 全路径。

---

## 2. 已经装进容器的东西（保留着，可直接用）

| 路径（容器内） | 内容 |
|---|---|
| `drive_c/windows/system32/wintab32.dll` | 自制 DLL，185,607 B，sha256 `e2bc6913…` |
| `drive_c/wt_pmax.txt` | `4095`（DLL 只在加载时读一次） |
| `drive_c/Test/wtprobe.exe` | 探针，151,598 B |
| `drive_c/SAI2/sai2.exe` | 你的 SAI 本体 |
| `user.reg` | 加了 `[Software\\Wine\\DllOverrides] "wintab32"="native,builtin"` 和 `[Environment] "WT_DEBUG"="1"` |

**用注册表替代容器环境变量**是这轮的实用发现：`DllOverrides` 等价于 `WINEDLLOVERRIDES=wintab32=n,b`，
`HKCU\Environment` 里的值会被 wine 灌进进程环境，所以不必改容器配置、不必重启 App。

---

## 3. 踩到的坑（写进检查单，别再踩）

### 3.1 在 adb 里跑 wine 的两个坑

1. **别把 `LD_LIBRARY_PATH` 全局 export**。rootfs 的 `usr/lib/libc.so` 是**链接脚本**不是 ELF，
   Android 的 toybox 命令（`rm`/`sleep`/`cat`/`ps`/`grep`/`head`）会因为 `LD_LIBRARY_PATH`
   去链它，全部报 `bad ELF magic: 2f2a2047`（`2f2a2047` 就是 ASCII `/* G`）。
   → 只在 `env ...` 前缀里给 box64 那一个进程设。
2. **`su 10310` 也过不了 wine 的 prefix 归属检查**。`id` 和 `stat` 都显示 10310 对 10310，
   wine 仍报 `'/data/.../.wine' is not owned by you` —— 说明 **box64 里的 `getuid()` 不可靠**。
   → 干净做法：用一个 **root 自己拥有的独立 prefix**（`/data/local/tmp/wptest`）做这类验证，
   与 App 的容器彻底隔离。

### 3.2 容器崩溃（23:21）：很可能是我的操作导致的

我在 root 下起了**第二个 wineserver**，并把它的 `DISPLAY` 指向了**正在运行的容器的 X 服务器**
（`rootfs/tmp/.X11-unix/X0`）。容器随后退出，回到 Containers 列表。
崩溃缓冲区里**没有** Winlator 的 Java/native 崩溃记录（只有我 23:14 那批 toybox 的报错），
因此更像是 Winlator 自己实现的 X 服务器被这个外来 client 搞挂了。

**处置**：已删除测试 prefix（314 MB）、移除我加的 `Run` 键、保留 DLL 与配置，重启容器 → 已恢复正常（见截图）。

**教训**：**不要拿第二个 wine 去连容器的 X 服务器**。要做加载/协议测试，用独立 prefix + 不设 DISPLAY；
要测"窗口 + 消息 + 取包"，就在容器里由 SAI 或探针自己开窗口。

---

## 4. 链路补测：**全部打通**（23:31）

用户在容器里装好了 SAI（在 `C:\SAI Ver.2\`）并打开了探针。检查发现：

- `sai2.exe`（pid 26730）进程里**映射着我们装的 DLL**：
  `/data/user/0/.../drive_c/windows/system32/wintab32.dll` ✅
- SAI 的 `Options → Pen Tablet` 里当前 API 就是 **`Use WinTab API`** ✅

从 adb 用 `nc -u` 发真实 UDP 报文：

```
before:  recv=2   posted=2   fetched=2
发送 25 个样本（压力 200→4000，位置斜线）
after:   recv=27  posted=27  fetched=27   gaps=0
```

DLL 日志：

```
producer: open=1 hwnd=000000000003019E posted=27 recv=27 gaps=0 fetched=27 udp=quiet
HOLD so far: ended by click=0 time-limit=1 pen-lifted=0
```

- `hwnd=0x3019E` 是 **SAI 的窗口**（不是探针的）
- `gaps=0` → 25 个包**一个没丢**
- `HOLD ... time-limit=1` → 首个按下包按设计等点击 30ms 后自动投递

**结论：`UDP 报文 → DLL 解析 → PostMessage 给 SAI → SAI 调 WTPacket 取走` 四段全部在真机上验证通过，一个包不丢。**

### 一个必须记住的副作用

合成样本里我只发了"按下"，没发"抬笔"，也没有真实鼠标点击配合，SAI 随后弹了：

```
Discharging of history data into file took a fatal error.
#1 ERROR! - A file I/O error has occurred.
#2/#3 ERROR! - Windows function call has failed.
```

补发压力 0 的悬停包后错误消失、SAI 恢复。**原因不是磁盘满**（`/data` 还有 125 GB），
是 SAI 的笔画由"点击"启动、历史记录按笔画推进；没有点击的假按下会让它卡在异常状态。
真机上触摸本身带点击，不会出现这个现象——但**做合成测试时一定要成对发送 按下/抬起**。

---

## 5. 压感硬件：这台手机上真的有

```
/dev/input/event8  "WH851-545 Pen"
  ABS_PRESSURE: min 0, max 16383
  AbsState: ABS_X=... ABS_Y=... ABS_PRESSURE=0 ABS_TILT_X=28 ABS_TILT_Y=-8
  Classes: KEYBOARD | TOUCH | EXTERNAL
  Sources: KEYBOARD | MOUSE | STYLUS
  IsExternal: true    AssociatedDisplay: <none>
```

- 有**真压感轴**（16384 级）和**倾斜**，比 Winlator 补丁里只取压力还好一档。
- 但 `AssociatedDisplay: <none>` → 这是**外接 BLE HID 笔**，Android 并没有把它绑定到某块屏。
  所以它的事件**会不会**以 MotionEvent 交给 Winlator、坐标在什么空间里，必须先抓一次原始事件才能确定。

**这也是为什么先不要急着编 APK**：如果不先确认笔的数据形态就动手，
很可能编完才发现坐标空间对不上。


---

## 5.5 真压感闭环：**数位板已经在驱动 SAI 了**（23:45）

用户接的是**外接数位板**（BLE HID，`/dev/input/event8`）：

```
ABS_X        min 0     max 40640
ABS_Y        min 0     max 25400     (16:10)
ABS_PRESSURE min 0     max 16383     ← 16384 级
ABS_TILT_X/Y min -127  max 127
keys: BTN_DIGI / BTN_TOUCH / BTN_STYLUS / BTN_TOOL_RUBBER
采样: 位置帧间隔中位数 1.35 ms ≈ 740 Hz
```

### 做法：不用 APK、不用 Gradle、不用 NDK

`patch/PenSender.java` —— 直接读 evdev，按 DLL 的报文格式发 UDP。
用 `javac`（JDK 25）+ `d8`（build-tools 36）编成 dex，用 Android 自带的 `app_process` 以 root 运行：

```sh
export CLASSPATH=/data/local/tmp/pensender.dex
app_process /system/bin PenSender /dev/input/event8 1280 720 4095 250
```

### 实测结果

```
recv=3101   posted=3091   fetched=3091          ← 一个不丢（差 10 是关节时的悬停包）
压力值采样: 300 499 670 909 1282 1597 1816 1869 1935 2072 2321 2385 2431
延迟: 0ms×10, 1ms×34, 2ms×4, 39ms×1       ← 44/49 在 2ms 内
```

**`数位板 evdev → PenSender → UDP → wintab32.dll → WT_PACKET → SAI` 全链路在真机上跑通，笔压是真的。**

### 最后一块拼图：笔画需要"按住的鼠标键"

SAI 用鼠标点击启笔画。三个 `input tap` 各画出一小团墨——因为 Winlator 的 tap 只给约
**30ms** 的按下（`handleFingerUp` 里 press 后 30ms 才 release），我们的压力包刚好在这 30ms
里被用上，所以有墨。

但 `input motionevent DOWN` 按住 25 秒**没有**产生任何笔画 —— 说明容器当前是
**Touchpad 模式**：按下不点击，点击只发生在"抬起且判定为 tap"时。

所以剩下的问题是纯输入层的：**让"按下"在整条笔画期间保持**。两条路：

1. **切到 Touchscreen（绝对）模式**：按住即按下，PenSender 只需在 tip 变化时注入
   `DOWN`/`UP`（每次笔画 2 次进程调用，可接受），不用编任何东西。
2. **打 Winlator 补丁**：在 `TouchpadView` 里读 `getPressure()` 并保持按钮状态——就是最初设计的方案，
   但需要 NDK 才能从源码构建。

---

## 6. SDK 体检结果（本机）

| 组件 | 状态 | 路径 |
|---|---|---|
| platforms | ✅ **android-37.0** | `%LOCALAPPDATA%\Android\Sdk\platforms` |
| build-tools | ✅ **36.0.0**（含 `d8`/`apksigner`/`zipalign`） | 同上 `build-tools` |
| platform-tools | ✅ | 同上 |
| emulator / system-images | ✅ | 同上 |
| **NDK** | ❌ **没有** | — |
| **cmdline-tools（sdkmanager）** | ❌ **没有** | — |
| **Gradle** | ❌ **没有**（`~/.gradle` 都不存在） | — |
| JDK | ✅ 25（Studio 自带 JBR，`C:\Program Files\Android\Android Studio\jbr`） | — |

判定：

- **从源码构建 Winlator：当前不可用。** `winlator-app/app/src/main/cpp` 存在，必须 NDK；
  还缺 Gradle + AGP 依赖，以及 Winlator 的 assets。补齐约 1.5–2 GB 下载，且容易在 AGP/Gradle 版本上翻车。
- **编一个小的独立 APK：可用**（有 platform 37 + build-tools + JDK，只缺 Gradle，约 130 MB）。
- **完全不用编译：可行**。root 在手上，直接用 `Termux + Python` 读 `/dev/input/event8` 发 UDP 即可，
  这正是 macOS 方案里"原生助手"的等价物。

## 7. 下一步的取舍

| 选项 | 代价 | 说明 |
|---|---|---|
| **A. 先抓笔的原始事件** | 3 分钟（需要你拿笔在屏上划） | 确认坐标空间、压力量程、是否真的产生 MotionEvent。**不确认这个，后面全是猜** |
| **B. Termux + Python 发 UDP** | 20–30 分钟 | 不用编任何 APK，最快能在 SAI 里看到真实压感 |
| **C. 编独立 helper APK** | 1–2 小时 | 需要下 Gradle；比 Termux 干净，可分发给别人 |
| **D. 从源码构建 Winlator** | 半天起，风险高 | 当前缺 NDK/Gradle/assets |

