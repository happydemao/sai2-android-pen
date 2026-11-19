# PaintTool SAI Ver.2 → Android：可行性取证与落地方案

对象：`sai2.exe`
版本：**PaintTool SAI Ver.2 (64bit) Alpha.2026.10.19**，5,212,160 字节
SHA-256：`DBCC58C6F3FE42139BB5113D917542270AF3C03928536BA3ACF55276F16423C5`
分析人：DSH Agent　｜　方法：静态 PE 取证（未运行、未修改）

---

## 0. 结论先行

| 问题 | 结论 |
|---|---|
| 能否把 sai2.exe "编译移植"成 Android 原生 APK？ | **不能。** 闭源商业软件，无源码；且它是 Win32/x64 + GDI 代码，没有可"移植"的源码层 |
| 能不能在安卓上**用**它？ | **能，而且成功率不低。** 走 Winlator（Wine + Box64）运行时路线，本质是"把这个 exe 搬进安卓里的 Windows 环境" |
| 关键障碍是什么？ | ① **笔压感**（最大硬伤）② 软件渲染的性能 ③ 桌面 UI 在小屏/触控下的尺寸 |
| 最优解是什么？ | 分场景：**画画为主 → 路线 D（串流/远程）体验最好**；**离线必须有 → 路线 A（Winlator）**；**要长期产品化 → 路线 C（重写）** |

一句话：**"移植"在这个软件上不成立，但"搬迁"成立。** 本文给四条可执行路线 + 已完成的二进制取证。

---

## 1. 二进制取证（为什么说它适合跑在 Wine 上）

全部由 `tools/inspect-pe.ps1` 复现，可自行验证。

### 1.1 基本属性

| 项 | 值 | 含义 |
|---|---|---|
| Machine | `0x8664` | x64（AMD64）→ 安卓侧需要 **Box64 二进制翻译**，不能直接跑 |
| Subsystem | `2` | GUI（Windows 程序，无控制台） |
| ImageBase / 重定位表 | `0x140000000` / **Reloc 缺失** | *非* ASLR 友好（`DYNAMIC_BASE` 置位但无 `.reloc`）→ Wine 需按固定基址加载，通常无碍，个别情况下会加载失败 |
| TLS 段 | 有（`.tls`，14,945 B） | 用了线程局部存储，Box64/Wine 支持 |
| 编译器 | MSVC，路径 `c:\myprojectsvs2010\systemax\sai2\sai2\canvas.src.integ.h`、`C:\MyProjectsVS2010\SYSTEMAX\sai2\bin64\sai2.pdb` | Visual Studio 2010 时代代码库，被后续持续编译 |
| 打包/加壳 | **无** | `.text` 熵 6.44（正常编译产物），导入表完整、明文大量可读字符串 → 未加壳，**无需脱壳即可运行** |

### 1.2 导入依赖：极度"干净"

只依赖 **Windows 核心系统 DLL**，总共 14 个、约 363 个函数：

```
KERNEL32(156) USER32(126) GDI32(29) SHELL32(12) ole32(13) ADVAPI32(8)
IMM32(8) SHLWAPI(3) COMDLG32(2) COMCTL32(2) OLEAUT32(2) mscms(1) RPCRT4(1) USERENV(1)
```

这带来三个重要推论：

1. **没有 `msvcr*.dll` / `vcruntime*.dll` / `api-ms-win-crt-*`** → C 运行库静态链接，**不需要装 VC++ 运行库**，Winlator 里免配置。
2. **没有 `d3d9/d3d11/dxgi/opengl32/ddraw`** → 不走 GPU API，**不需要 DXVK/VKD3D/Turnip 调优**，也不会因为显卡驱动翻车。
3. **没有 `gdiplus/windowscodecs/libpng/libjpeg`** → 图像编解码是自带的（二进制内有 zlib 的错误字符串 `Extra compressed data` / `Incomplete compressed datastream` 等），**不依赖系统解码器**。

> 结论：这是一个"只吃 Win32 + GDI 的最小闭包"程序。Wine 对这套 API 的实现成熟度最高，**这是它能在安卓上跑起来的根本原因**。

### 1.3 渲染方式：纯 GDI 软件绘制

GDI32 实际导入的关键函数：

```
CreateDIBSection, CreateCompatibleBitmap/DC, BitBlt, SetDIBitsToDevice,
ExtTextOutW, SelectObject, ExtCreateRegion, CombineRgn, ExtSelectClipRgn,
CreateRectRgnIndirect, SetBkMode, CreateFontIndirectW ...
```

含义：
- 画布是 **DIB（先画到内存位图，再整块 BitBlt/SetDIBitsToDevice 到窗口）**，没有 GPU 加速。
- 好处：不碰显卡驱动，转换层风险低，画面不会花。
- 代价：**每帧的像素搬运由 Box64 翻译 x86 指令完成**，是性能瓶颈所在；大画布 + 高刷新笔迹会掉帧。

### 1.4 笔输入：双通道（Windows Ink + Wintab）

静态证据（导入 + 字符串）：

```
GetPointerPenInfo, GetPointerPenInfoHistory          (WM_POINTER / Windows Ink，动态解析)
WM_POINTERUPDATE / WM_TABLET_* / WMX_TABLET_PENPOS / WMX_TABLET_PROXIMITY
WM_PENWINFIRST / WM_PENWINLAST
GetMessageExtraInfo, SendInput, mouse_event          (鼠标回退路径)
wintab32.dll                                         (动态 LoadLibrary，Wintab 兼容层)
  └ "AvoidWacomBug" / "AlwaysUseDefaultContext" / WTInfo(WTI_CURSORS...CSR_SYSBTNMAP) / "puck" / "eraser"
```

含义分两面：

- ✅ 它**原生支持压感**，而且是**两条腿走路**：新式 Windows Ink（`WM_POINTER`）+ 老式 Wintab（`wintab32.dll`，含 Wacom 兼容修补标志）。另有鼠标回退路径，所以无压感环境也能画。
- ❌ 安卓侧 Winlator 的输入是"触摸 → 鼠标事件"的转换，**既不产生 `WM_POINTER` 的 pen 信息，也没有 Wintab 设备**。所以跑起来后 SAI 会认为你在用**鼠标**：有笔迹、**没有压感**，笔刷恒定粗细/不透明度。

> 但双通道这点**很重要**，它意味着压感在安卓上是**可以补的**：
> 社区已有 macOS 版的现成实现（自制 `wintab32.dll` + 一个读笔压的助手，UDP 喂样本），
> 而那个 DLL 是 Win64 二进制，在 Box64 里能原样跑。安卓缺的只是"助手"那半——
> 因为安卓不给 App 全局触摸监听，压力只能从**前台的 Winlator 自己**身上取。
> 完整设计与已验证的报文格式见 **[ANDROID-PRESSURE-PORT.md](./02-bridge-design.md)**。

### 1.5 运行时数据布局（影响迁移方案）

```
%APPDATA%\SYSTEMAX Software Development\SAIv2\   ← 设置、笔刷、色板、恢复数据
  sai.ssd                     ← 全局设置/系统 ID
  *.ini                       ← 各面板布局
  共享内存 PaintToolSAI-{52E99E19-…}-SharedMemory / -Cleanup  （单实例互斥）
文件格式：*.sai（Ver.1）、*.sai2、SAI-* 自定义块（SAI-CANVAS / SAI-SWATCH / SAI-COLHIS / SAI-SPAD）
```

推论：
- **单文件程序**：把 `sai2.exe` 一个文件拷过去就能启动，不挑目录、不装组件。✅
- 但设置/笔刷/色板都在 `%APPDATA%` 里，**第一次启动是"全新 SAI"**，要重新导入笔刷和色板（从 PC 拷 `SAIv2` 整个文件夹过去即可）。⚠️
- 支持 `*.sai` / `*.sai2` 原生格式读写 → 与 PC 互通没问题。

---

## 2. 四条路线对比

| | A. Winlator 运行原程序 | B. Winlator + 自制压感桥 | C. 重写成安卓原生 App | D. 串流 / 远程桌面 |
|---|---|---|---|---|
| 本质 | 安卓里搭一个 Windows 兼容层 | 同 A，再把笔压接进去 | 真正意义上的"移植" | 算力留在 PC，画面投到平板 |
| 可行性 | ★★★★☆ 高 | ★★★☆☆ 中（要能编 APK） | ★☆☆☆☆ 极低（无源码） | ★★★★★ 最高 |
| 上手时间 | 30–60 分钟 | 1–2 天（含环境） | 6–12 人月 | 20 分钟 |
| **压感** | ❌ 无 | ✅ **有**（自制 `wintab32.dll` + Winlator 补丁） | ✅ 有（MotionEvent.getPressure） | ✅ 有（完整笔压 + 倾斜） |
| 性能 | 中低（Box64 翻译 GDI 搬运） | 同 A | 高 | 取决于网络，本地 Wi-Fi 近满帧 |
| 触控/UI | 桌面 UI，需触屏映射，小按钮难按 | 同 A | 可重设计 | 同样的桌面 UI，但屏幕大（平板） |
| 法律风险 | 低（个人自用） | 低（桥是 GPL-3.0） | **高**（需逆向文件格式，需自己写全部算法） | 低 |
| 能否改代码 | ❌ | ✅（改 Winlator 输入层） | ✅ | ❌ |

**建议组合**：先用 **A** 验证"能不能用"；要压感再上 **B**（详见 [ANDROID-PRESSURE-PORT.md](./02-bridge-design.md)）；日常画画优先评估 **D**；只有当你要做产品才考虑 **C**。

---

## 3. 路线 A：Winlator 落地（推荐先做这个）

完整操作见 [`WINLATOR-QUICKSTART.md`](./08-winlator-quickstart.md)。要点摘要：

- 用 **Winlator 11.2**（brunodev85 官方版，2026 年当前主线）。它基于 Wine + Box64，**支持 x86_64 程序**，正好对上 sai2.exe 的 AMD64。
- 容器里 Graphics Driver 随便选（SAI 不用 GPU API）；**关键是 Box64 preset 用 `Compatibility`/`Stability`**，因为 SAI 大量用 SSE 做像素运算，激进优化容易崩。
- 屏幕 DPI 建议 **120–144**（SAI 是 DPI-aware，96 会让按钮窄到手指按不住）。
- 把 `sai2.exe` 放进容器 `C:\SAI2\`，双击即启动；想保留笔刷就一起拷 `SAIv2` 设置目录。

预期结果（诚实预估）：
- ✅ 能启动、能画、能存 `.sai2`、能导出 PNG
- ⚠️ 笔迹有延迟（毫秒级到几十毫秒，看 CPU 和画布大小）
- ❌ 没有压感；只能用固定笔刷 + SAI 的"笔压"参数失效
- ⚠️ 小屏手机基本没法用；**8 寸以上平板 + 触控笔（当鼠标用）才现实**

---

## 4. 路线 B：在 Winlator 里补上压感（详见独立文档）

思路：SAI 的压感走的是 **WinTab**（它动态加载 `wintab32.dll`，见 1.4）。所以只要在容器里放一个
**自制的 `wintab32.dll`** 来供笔压，SAI 就完全不改；DLL 需要的笔样本通过 `127.0.0.1:47800` 的 UDP
文本报文送进去。安卓侧唯一的问题是谁来发——答案是 Winlator 自己（它是前台，握着 `MotionEvent`）。

社区已把这套在 macOS 上跑通（GPL-3.0 授权），DLL 是 Win64 二进制，Box64 可原样运行。

- 完整设计、协议、逐块可行性、以及"为什么安卓写不出独立助手"：**[ANDROID-PRESSURE-PORT.md](./02-bridge-design.md)**
- 补丁位置、编译、验收、排障：**[03-wintab-dll-protocol.md](./03-wintab-dll-protocol.md)**
- 安卓发送端源码：**[patch/SaiPressureBridge.java](../android/SaiPressureBridge.java)**
- 桌面先验通的工具：**[tools/sai_pen_test.py](../tools/sai_pen_test.py)**

代价：要能编译 APK；即使压感通了，Box64 翻译 GDI 的笔迹延迟仍在。

---

## 5. 路线 C：如果真要"重写一个 SAI"

**先明确：这不是移植，是照着它写一个新软件。** 没有源码，只有可观察行为 + 文件格式。

要复刻的最小可用集（MVP）：

1. **画布与合成**：`Bitmap`/`ImageBitmap` + `RenderNode`，或 C++ + Skia（`libskia` 与安卓原生兼容），分层（Layer/Group），图层混合模式。
2. **笔刷引擎**（SAI 的灵魂，也是最难的部分）：
   - 笔压 → 笔尖半径/不透明度/浓度映射
   - **水彩混色（Watercolor）**：SAI 的"模糊/水彩边界/扩散"是全网最难复刻的算法
   - 笔刷纹理、抖动、纸质
3. **输入**：`MotionEvent` 的 `getPressure()` / `getAxisValue(AXIS_TILT)` / `getHistorical*` 做插值；`PointerCoalescing` 保笔迹密度；低延迟用 `SurfaceView` + `requestUnbufferedDispatch`。
4. **文件格式**：`.sai2`/`.sai` 是私有二进制（`SAI-CANVAS-TYPE` 等块），需要**逆向解析**才能与 PC 互通；否则只能自定义格式 + 导出 PNG/PSD。
5. **UI**：SAI 的极简紧凑面板在 10 寸以下需要重排，不能照搬。

现实评估：一个人业余做，**能画出线条的 MVP 约 2–3 个月**；做到"接近 SAI 手感"（水彩、混色、笔刷手感）**1 年以上**，且必须解决 `.sai2` 兼容（否则老用户不会迁移）。

如果目标是"在安卓上有 SAI 的体验"，**建议直接选现有替代品**（Krita / HiPaint / ibisPaint / Infinite Painter / 概念画板），把精力放在工作流上。这不是敷衍——这是投入产出比最高的结论。

---

## 6. 路线 D：串流（体验最优，强烈建议配合）

- PC 端：**Sunshine**（开源 GameStream 主机）
- 安卓端：**Moonlight**（平板/手机）
- 效果：SAI 跑在 PC 上，平板当"数位屏 + 触控"，**压感、倾斜、性能、全部原样保留**，局域网 1080p60 基本无感延迟。
- 这才是"用安卓画画 + 用 SAI"的最优解，且 20 分钟能配好。
- 变体：Parsec、Steam Link、RDP（RDP 不传压感，不推荐画画）。

---

## 7. 验收清单（做完 A 之后自查）

- [ ] `sai2.exe` 在容器内启动，不再提示缺少 DLL
- [ ] 新建画布、画一笔、撤销/重做正常
- [ ] 保存 `.sai2` 后拷回 PC 能打开
- [ ] 导出 PNG（含透明）正常
- [ ] 中文/日文 UI 不乱码（Wine 字体）
- [ ] 触控笔能画，且不会误触发右键/滚动
- [ ] 长按笔迹不丢（Box64 稳定性）
- [ ] 大画布（3000×3000 以上）拖动不崩

---

## 8. 后续可以做什么

按投入从小到大：

1. **补上 letterbox**：现在数位板整面被拉伸到容器整面，16:10 的板子配 16:9 的容器会有轻微纵向
   拉伸。保持比例 + 居中即可修掉，改的是 `PenBridge` 里的坐标映射。
2. **转发倾斜与橡皮**：evdev 里 `ABS_TILT_X/Y`、`BTN_TOOL_RUBBER` 都有，但 `MotionEvent` 侧没转发，
   报文协议也没有对应字段——要扩协议并重编 DLL。
3. **.sai2 文件格式解析**：重写路线的第一步，也是唯一能立刻动工的硬骨头。
4. **串流配置**：Sunshine + Moonlight 的完整参数（含防火墙、码率、平板手势），适合"画画为主"的场景。
5. **把补丁从 smali 搬到源码**：目前是"反编译原版 APK 改 dex"，长期应该做成 Winlator 的 fork，
   在 `TouchpadView.java` 里直接改，跟着上游走。

## 附录：本次取证用料

- 文件哈希、PE 解析、导入表、各段熵、UTF-16 字符串 —— 全部由 `tools/inspect-pe.ps1` 现场重算，不依赖外部工具。
- 复现命令（注意：Windows PowerShell 默认禁止运行脚本，需 `-ExecutionPolicy Bypass`；文件为 **UTF-8 with BOM**，请勿用会去掉 BOM 的编辑器另存）：

  ```powershell
  powershell -NoProfile -ExecutionPolicy Bypass `
      -File .\tools\inspect-pe.ps1 -Path sai2.exe
  ```

  加 `-FullImports` 可打印全部 363 个导入函数名。
- 未执行、未修改 `sai2.exe`。分析时的临时副本已删除。
