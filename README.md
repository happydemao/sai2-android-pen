# PaintTool SAI Ver.2 真压感 · Android / Winlator

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](./LICENSE)
[![Platform](https://img.shields.io/badge/platform-Android%20%2B%20Winlator-3ddc84.svg)](#)
[![No root](https://img.shields.io/badge/root-not%20required-success.svg)](#已验证的能力)

把 **Windows 版 PaintTool SAI Ver.2** 搬到 Android（Winlator + Wine + Box64）上跑，
并且把外接数位板的**真实笔压**送进 SAI —— 不改 SAI 本体、不 root、不打命令。

> 这不是"移植"。SAI 是闭源商业软件，没有源码可移植；这是**搬迁**——把它放进 Android 上的
> 一个 Windows 兼容层，然后给它补一条它本来就支持的输入通道（WinTab）。


## 它是怎么工作的

SAI 自己支持 Wintab 数位板协议，Wine 自带的那份是空实现。所以压力不需要"hack 进 SAI"，
只需要**在 Wine 里放一个真的 `wintab32.dll`**，再从 Android 侧把压力喂给它：

```
  数位板 / S Pen
        │  MotionEvent（pressure 0..1，Android 给的是归一化值）
        ▼
  Winlator 的 TouchpadView  ──►  PenBridge（编进 APK 的 classes2.dex）
        │                               │
        │  常规触摸→鼠标（位置、点击）    │  "seq pressure x y w h"  UDP → 127.0.0.1:47800
        ▼                               ▼
   Wine/XServer 里的指针          wintab32.dll（Box64 执行，Wine 加载进 sai2.exe）
        │                               │  WT_PACKET / WTPacket
        └──────────────► sai2.exe ◄─────┘
                          画布：粗细随力度变化
```

三个关键结论（都是实测出来的，见 [docs/06-device-test-log.md](./docs/06-device-test-log.md)）：

1. **SAI 是"只吃 Win32 + GDI 的最小闭包"**：不依赖 VC++ 运行库、不碰 D3D/OpenGL、自带图像编解码，
   所以 Wine 对它成熟度最高，不需要 DXVK/Turnip 调优。（PE 取证见 [docs/01-feasibility.md](./docs/01-feasibility.md)）
2. **上游 macOS 方案的 DLL 在 Box64 下能原样加载**：UCRT 依赖、Winsock 回环、消息钩子、WinTab 握手全部通过。
3. **报文是纯文本**，格式简单到可以手写：`"seq pressure x y w h"`，UDP 单播到回环。

## 快速开始

完整图文教程：[docs/09-install-tutorial.md](./docs/09-install-tutorial.md)。
视频/图文版（作者录制）：[安卓终于可以用sai2力 · B站](https://www.bilibili.com/opus/1272346716743401491)。

**从 0.3.0 起，除了 SAI 本体，其它东西都内置在 APK 里**（压感桥、`wt_pmax.txt`、注册表覆盖、
Noto 中文字体）。所以要做的只有三步：

1. **装补丁版 Winlator**（[Releases](/releases) 里的 `Winlator-11.2-SAIPen.apk`）。
   > ⚠️ 包名仍是 `com.winlator`，签名不同：装之前必须卸载官方版，**容器和里面的东西会没**。
2. **建容器**：分辨率按屏幕填，DPI 调大些，并**取消勾选 `Capture pointer on external mouse`**。
3. **放入你自己的 SAI**（本项目不提供，见[法律边界](#法律与许可)），完整目录、保留 `init/`，
   双击 `sai2.exe` 即可。

验收：`C:\wt_status.txt` 里 `open=1`，落笔画线粗细随力度变化。

> 用旧版 APK、或要给**升级前就存在**的容器补桥，见教程的
> [附录 A](./docs/09-install-tutorial.md#附录-a旧版-apk--已有容器手动补桥)（手动放 DLL + 导入两个 `.reg`）。

## 仓库结构

```
bridge/     Windows 侧：wintab32.c / wintab_core.h / wintab32.def（Wintab 实现 + UDP 收样本）
android/    安卓侧：PenBridge.java（编进 APK 的发送端）、build-apk.ps1（一键重建 APK）
            scripts/：容器内排查用的一次性 adb 脚本（历史存档，按需取用）
dist/       预编译产物：wintab32.dll、wtprobe.exe、wt_pmax.txt、两个 .reg 注册表片段
tools/      分析/自检工具：PE 取证、报文模拟发送、端到端校验、wt_status 解读
docs/       设计、协议、打补丁、实测、排障、教程
box64.env.example   容器环境变量模板（先只加标 ★ 的 4 条，能跑再按需加）
```

## 文档

| 文档 | 内容 |
|---|---|
| [01-feasibility.md](./docs/01-feasibility.md) | 可行性取证：PE 结构、导入依赖、GDI 渲染、WinTab 双通道、四条路线对比 |
| [02-bridge-design.md](./docs/02-bridge-design.md) | 压感桥的整体设计：为什么只能搬一半、"假 macOS 助手"方案、取舍 |
| [03-wintab-dll-protocol.md](./docs/03-wintab-dll-protocol.md) | **报文协议** + DLL 用法 + 四处 smali 改动 + 编译 + 验收顺序 |
| [04-patching-winlator.md](./docs/04-patching-winlator.md) | 打补丁实录：为什么只用 `{p0,p1}`、笔点击的原理、坑与回滚 |
| [06-device-test-log.md](./docs/06-device-test-log.md) | 真机实测全记录，含原始日志与逐项结论 |
| [07-status-and-limits.md](./docs/07-status-and-limits.md) | 当前可用形态、已知限制、后续可优化项 |
| [08-winlator-quickstart.md](./docs/08-winlator-quickstart.md) | 不追求压感时的最小可用配置与性能调优 |
| [09-install-tutorial.md](./docs/09-install-tutorial.md) | **图文安装教程**（新手向，三步装完） |
| [10-releasing.md](./docs/10-releasing.md) | 发布清单：Release 附件、LGPL 义务、不要提交什么 |
| [11-bluetooth-keyboard-fix.md](./docs/11-bluetooth-keyboard-fix.md) | 蓝牙键盘不进容器：复合 BLE 键盘被误判成手柄的根因与修复 |
| [12-wheel-zoom.md](./docs/12-wheel-zoom.md) | 鼠标滚轮改为以光标为中心缩放画布 |
| [13-pen-cursor-follow.md](./docs/13-pen-cursor-follow.md) | 系统鼠标光标跟随内置笔的悬停/落笔 |
| [15-touch-gestures.md](./docs/15-touch-gestures.md) | 触控屏手势：两指撤销、三指重做、平移/缩放/旋转、快捏适合窗口 |
| [16-winlator-chinese.md](./docs/16-winlator-chinese.md) | Winlator 主界面汉化：中文资源、zh_CN 语言项、安装与切换 |
| [17-branding.md](./docs/17-branding.md) | 应用名与启动图标：winlator for sai |

## 已验证的能力

| 能力 | 状态 |
|---|---|
| SAI 在 Winlator 里运行、画布正常 | ✅ |
| 数位板压感进入 SAI（实测 1 ms 中位延迟、gaps=0） | ✅ |
| 发送端编进 APK，无 root、无命令、无常驻进程 | ✅ |
| 用笔点 UI（任务栏、菜单、工具栏） | ✅ 两种笔都验证过（BLE 笔 / S Pen） |
| 外接键盘 | ✅ USB 直接可用；复合 BLE 键盘（MCHOSE G75 V2）已修，0.3.4 起内置，见 [11](./docs/11-bluetooth-keyboard-fix.md) |
| 鼠标滚轮缩放画布 | ✅ 0.3.5 起，滚轮映射为 PageUp/PageDown，见 [12](./docs/12-wheel-zoom.md) |
| 系统光标跟随笔 | ✅ 0.3.6 起，悬停/落笔都同步系统光标，见 [13](./docs/13-pen-cursor-follow.md) |
| 触控屏手势（两指撤销/三指重做/平移/缩放/旋转/快捏适合窗口） | ✅ 0.3.8 真机实测，见 [15](./docs/15-touch-gestures.md) |
| Winlator 主界面中文 | ✅ 0.3.8，318 条文案 + 语言列表「简体中文」，见 [16](./docs/16-winlator-chinese.md) |
| 应用名 / 图标 | ✅ 0.3.9，`Winlator for sai` + 新图标，见 [17](./docs/17-branding.md) |
| 虚拟鼠标按钮光标 | ✅ 0.3.9，按下时先把光标移到手指位置再点击，见 [13](./docs/13-pen-cursor-follow.md) |
| 手指操作 UI | ⚠️ 可用，双击仍偏难 |
| 倾斜 / 橡皮 | ⏸ 未做（evdev 有数据，协议无字段，需重编 DLL） |
| SAI 首笔弹 `FileError(161)` | ⚠️ SAI×Wine 兼容问题，非致命，不影响作画与撤销 |

已知限制与原因：[docs/07-status-and-limits.md](./docs/07-status-and-limits.md)。

## 自己构建

### `wintab32.dll`（交叉编译，任意平台）

```bash
# 需要 mingw-w64：brew install mingw-w64 / apt install gcc-mingw-w64-x86-64
cd bridge
x86_64-w64-mingw32-gcc -shared -O2 -o wintab32.dll wintab32.c wintab32.def \
    -lgdi32 -luser32 -lws2_32 -municode
```

`wintab_core.h` 是纯逻辑（解析/映射/合帧），不依赖 Win32，可以直接用 clang/gcc 本地编译跑单测。

### 补丁版 APK（需要 Windows + Android SDK）

`android/build-apk.ps1` 从 `base.apk` 出发：baksmali → 打 3 处 smali 补丁 → smali 汇编 →
`javac` + `d8` 编 `PenBridge.java` → 替换两个 dex → `zipalign -P 16` → `apksigner`。

```powershell
# 需要：你自己的 base.apk、debug.keystore、JDK 21、build-tools、smali/baksmali jar
.\build-apk.ps1 -BaseApk .\in\base.apk -SmaliTools .\in\smali-tools -Sdk C:\Android\Sdk -Jdk C:\jdk-21 -Install
```

脚本参数与每一步的作用见 [docs/04-patching-winlator.md](./docs/04-patching-winlator.md)。

## 协议（一句话版）

```
"<seq> <pressure> <x> <y> <w> <h>\n"   UDP → 127.0.0.1:47800
```

`pressure` 为整数（上限由 `C:\wt_pmax.txt` 决定，默认 4095），`x y` 是容器坐标，`w h` 是容器分辨率。
DLL 收到后按 WinTab 的轴映射表把样本给 SAI。完整语义、字段边界与排障见
[docs/03-wintab-dll-protocol.md](./docs/03-wintab-dll-protocol.md)。

## 法律与许可

- **本仓库不含 PaintTool SAI**，也不含它的安装包、授权证书或任何绕过方式。SAI 是 SYSTEMAX 的
  商业软件：作画免费，**保存文件需要你自己的授权**。
- 本仓库是 [SAI Pen Pressure（ametrien）](https://github.com/ametrien/Paint-Tool-SAI-pen-pressure-macOS-fix)
  的**衍生作品**（`bridge/` 与 `android/SaiPressureBridge.java` 来自该项目，已修改），因此整体以
  **GPL-3.0** 发布，并保留其 `NOTICE` 与署名。详见 [NOTICE](./NOTICE)。
- Release 里的 APK 是 **Winlator（LGPL-2.1-or-later）的非官方修改版**，内含 Wine、Box64 等。
  分发时须说明这是修改版并提供源码获取方式，见 [THIRD-PARTY.md](./THIRD-PARTY.md) 与
  [docs/10-releasing.md](./docs/10-releasing.md)。
- 与 SYSTEMAX、Winlator 作者均无关联，未获其背书。

## 致谢

- **[ametrien / SAI Pen Pressure](https://github.com/ametrien/Paint-Tool-SAI-pen-pressure-macOS-fix)**
  —— 整个思路、`wintab32.dll` 的实现、UDP 报文格式、"用压力自己判断落笔"的技巧，全部来自这个项目。
  没有它就没有这个仓库。
- **[Winlator](https://github.com/brunodev85/winlator)** —— Android 上的 Wine 容器，本项目的补丁对象。
- **[Box64](https://github.com/ptitSeb/box64)**、**Wine** —— 让 x86_64 的 SAI 在 arm64 上跑起来的那两层。
- **SYSTEMAX** —— PaintTool SAI。

## English

Run **PaintTool SAI Ver.2** (Windows, x64) on Android through **Winlator**, with **real pen
pressure** from an external tablet, without root and without modifying SAI.

SAI already speaks the WinTab tablet API, and Wine ships an empty stub for it. So the pressure
does not have to be patched into SAI: a real `wintab32.dll` is dropped into the Wine prefix, and an
Android-side sender (`PenBridge`, compiled into the Winlator APK) feeds it pen samples over UDP
(`127.0.0.1:47800`, plain-text `"seq pressure x y w h"`).

Verified on a Redmi K40 Pro (BLE pen) and a Galaxy Tab S9+ (S Pen): pressure reaches the canvas,
~1 ms median latency, and the pen can click SAI's UI.

Start with [docs/09-install-tutorial.md](./docs/09-install-tutorial.md) (Chinese) or
[docs/02-bridge-design.md](./docs/02-bridge-design.md) for the architecture.

**Not included:** PaintTool SAI itself, any installer or licence. Bring your own lawful copy.
This repository is a modified derivative of [SAI Pen Pressure by
ametrien](https://github.com/ametrien/Paint-Tool-SAI-pen-pressure-macOS-fix) and is therefore
licensed **GPL-3.0** — see [NOTICE](./NOTICE) and [LICENSE](./LICENSE).
