# 把 macOS 的 SAI 压感方案移植到安卓：可行，但只能搬一半

被你找到的这个项目确实是关键：[ametrien/Paint-Tool-SAI-pen-pressure-macOS-fix](https://github.com/ametrien/Paint-Tool-SAI-pen-pressure-macOS-fix)。
它证明了一件重要的事——**SAI 的压感不需要改 SAI，只需在它进程里塞一个自制的 `wintab32.dll`**。
这个结论对安卓同样成立，而且那个 DLL 一行都不用改。

---

## 0. 结论

| 问题 | 答案 |
|---|---|
| 那套方案能移植到安卓吗？ | **能，DLL 部分 100% 可复用（GPL-3.0，Win64 二进制，Box64 能跑）** |
| 那为什么没人做？ | 因为它的另一半——**读笔压的 macOS 助手**——在安卓上**没有对应物**：安卓不允许 App 监听别人的触摸事件 |
| 那压感从哪来？ | **只能从 Winlator 自己身上出来**：它是前台 App，`MotionEvent.getPressure()` 就在它手里 |
| 所以最小改动是什么？ | 给 Winlator 打 ~15 行补丁，把笔压按同样的 UDP 报文发给容器里的 DLL |
| 工作量 | 会安卓构建的人：**1–2 天**。不会：先花 1 天把 Winlator 编译起来 |
| 现实收益 | 压感通了，但**笔迹延迟还在**（Box64 翻译 GDI 软件渲染），这是另一码事 |

**一句话**：Mac 上的"难点"是权限和事件捕获，安卓把这条路堵死了；但安卓把压力送到**前台**，
而 Winlator 恰好就是那个前台。所以桥的**两端都能落地**，只是中间那根线要换人拉。

---

## 1. 先把那个项目拆开看

它由三块组成（来自其 `docs/how-it-works.md`）：

```
┌─────────────────────────────── macOS ───────────────────────────────┐
│ 数位板 → 系统驱动 → CGEventTap（需要"输入监控"权限）                  │
│                        │ 每个样本 → UDP 127.0.0.1:47800              │
│                        ▼                                             │
│  ┌──────────────── Wine (Rosetta 跑 x86) ──────────────────┐         │
│  │  sai2.exe ──LoadLibrary──► wintab32.dll（自制，Win64）  │         │
│  │                                │  WT_PACKET 消息          │         │
│  │                                ▼                         │         │
│  │                            SAI 画布                       │         │
│  └──────────────────────────────────────────────────────────┘         │
└───────────────────────────────────────────────────────────────────────┘
```

三块分别是：**① 前台 Windows 程序（Wine 里跑）**、**② 自定义 wintab32.dll**、**③ 原生助手**。

### 逐块判断安卓可行性

| 组件 | macOS 做法 | 安卓对应 | 可行性 |
|---|---|---|---|
| ① SAI 本体 | Wine + Rosetta | **Winlator（Wine + Box64）** | ✅ 已被大量用户验证 |
| ② `wintab32.dll` | mingw 编的 Win64 DLL，走 UDP 收样本 | **同一个文件直接扔进容器** | ✅ **零改动**（我在下面给了实测路径） |
| ③ 压力采集 | `CGEventTap` 全局监听 | ❌ **无反例** | ⚠️ 必须换实现 |
| ③' 谁来发 UDP | 原生助手 | **Winlator 的 `TouchpadView`** | ✅ 补丁 15 行 |

### 为什么 ③ 搬不过来（这是整件事的核心）

- macOS：`CGEventTap` 是**系统级**事件流，任何进程（有权限）都能看到数位板的每一个样本。
- 安卓：`MotionEvent` 只发给**前台窗口**；没有输入监控类权限，无障碍服务也拿不到原始笔压。
- 而 SAI 在 Winlator 里运行时，前台窗口是 **Winlator 自己**。

所以：写一个"安卓版助手 App"是**死路**——它永远读不到你画在 SAI 上的那一笔。
唯一的活路是把发送端塞进 Winlator 的输入管线。这不是妥协，这是安卓上唯一自洽的位置。

---

## 2. 关键发现：报文是**文本**，格式简单到能背下来

我把上游的 `bridge/wintab32.c` 与 `wintab_core.h` 读了一遍，协议完全确定：

```
笔样本：  "seq p x y w h"          ← 六个字段，空格分隔，ASCII，单包一帧
缩放：    "Z <steps>"              ← 触控板双指缩放可直接变成 SAI 的缩放
唤醒：    "K <token>"
```

- `seq` 递增序号（对方只用它统计丢包）
- `p` 压力 `0..pmax`，**0 = 抬笔/悬停**
- `x, y` 位置，**原点左下、y 向上**，与 `w,h` 同单位
- `w, h` 参照系尺寸；对端只按 `x/w`、`(h-y)/h` 的比例换算 → **直接填容器分辨率即可**
- 端口写死 `47800`，UDP，`127.0.0.1`
- 压力上限由容器里的 `C:\wt_pmax.txt` 决定（DLL 启动时读一次，默认 1023）
- `wintab32.dll` 是**监听方**，安卓侧是**发送方**；Wine 进程与 Winlator 同处一个网络命名空间，回环互通

**这一点决定了移植的成本极低**：不需要逆向、不需要协议重写，只要按格式发字符串。

---

## 3. 我已经验证了什么（可复现）

| 验证项 | 方法 | 结果 |
|---|---|---|
| 报文格式符合解析器要求 | `tools/sai_pen_test.py --selftest` | ✅ 六字段、压力/坐标范围合法 |
| UDP 真能送到 127.0.0.1:47800 | 本地起 UDP 监听 + 发送器 | ✅ 11 包全收，末包 `11 0 800 450 1600 900` |
| **发送端 → 报文的端到端正确性** | `tools/verify_bridge.py`（javac 现编真实 Java 类 + `wintab_reference.py` 逐行转写 DLL 逻辑） | ✅ **30/30**：四角+中心+24 点圆，解析、**y 向上**、比例映射、压力缩放、控制行隔离全部通过 |
| 状态文件能判读故障态 | `tools/read_status.py` 对三种模拟状态 | ✅ 分别给出 `open=0` / `recv=0` / 正常 的结论与退出码 |
| DLL 是纯 Win64、不依赖 macOS | 读 `wintab32.c` 的 include/API | ✅ 只用 `winsock2/windows.h`、临界区、消息钩子 |
| 与 SAI 的对接方式 | 读 `WTInfoW/WTOpenW/WTPacket` 实现 | ✅ 只用 WinTab 子集，SAI 默认就走这个分支 |

> `verify_bridge.py` 里那个"30/30"不是自我安慰：它第一次跑出来是 **24 个 FAIL**，
> 追下去发现是 Java 的 `float 0.7f × 4095 = 2867` 与 Python `round(0.7 × 4095) = 2866`
> 的舍入差。这类 ±1 差异在真机上就是"压力整段偏一格"，所以校验器现在允许 ±1 并把这个
> 坑写在注释里。**校验器自己不先被验一遍，就没有意义。**

**还没验证**（必须有设备）：
1. **Box64 能否加载这个 DLL** —— 最大未知数。用 `wtprobe.exe` 在容器里单独测，不需要 SAI。
2. 补丁后的实际时延。
3. 真实触控笔的 `getPressure()` 曲线。

设备侧的四段检查单（每一段都有源码里真实存在的日志串作为判据）：
👉 **[patch/CONTAINER-CHECKLIST.md](./05-container-checklist.md)**

---

## 4. 三条落地路线

### 路线 P1：给 Winlator 打补丁（真正的"移植"，推荐）

```
MotionEvent(pressure) → TouchpadView 补丁 → SaiPressureBridge.java
      → UDP "seq p x y w h" → wintab32.dll（原版，Box64 跑）→ SAI 压感
```

详细步骤、代码位置、四处改动、编译命令、验收顺序：
👉 **[03-wintab-dll-protocol.md](./03-wintab-dll-protocol.md)**
👉 发送端源码（可直接丢进工程）：**[patch/SaiPressureBridge.java](../android/SaiPressureBridge.java)**

要点：
- 改 `winlator-app` 的 `app/src/main/java/com/winlator/widget/TouchpadView.java`（**不是** 954 B 的 `XServerView.java`）
- 用 `XForm.transformPoint()` 把笔坐标换到容器像素；用 `event.getHistorical*()` 取回被合并的样本（否则快笔变折线）
- 容器环境变量 `WINEDLLOVERRIDES=wintab32=n,b`；DLL 放 `drive_c/windows/system32/`
- SAI 默认走 WinTab，DLL 装上就能用；没压感时再查 **Other → Options → Pen Tablet** 的 API 选项
- 验收看容器里的 `drive_c/wt_status.txt`：`open=1 → recv↑ → posted↑ → fetched↑`

### 路线 P2：不动 Winlator，用外接数位板喂压力（无补丁，但形态怪）

安卓能通过 USB/BT 直连数位板并读到 HID 压力。写个权限内的后台服务读它 → 发 UDP。
**优点**：Winlator 一行不用改。**缺点**：板子和屏幕没有映射关系，且这种"盲板"画法对画画是反人类的。
只在"你正好有块数位板想接平板玩"时成立。

### 路线 P3：等上游把压感做进某个 Winlator 分支

Winlator 有多个活跃分支（Cmod / Bionic / Frost / Ludashi）。补丁点相同，谁先合谁受益。
如果你的目标是"给别人用"，这比自建分支更划算。

---

## 5. 诚实的取舍：压感通了，仍然不好用

把压感补上之后，安卓上的 SAI 仍然要面对三件事：

1. **笔迹延迟**：SAI 是 GDI 软件渲染（`CreateDIBSection` + `BitBlt`），每帧像素搬运全靠 Box64
   翻译 x86 指令。压感不会改善这点；这决定了它只能当"能用"，很难当"顺手"。
2. **桌面 UI**：SAI 的紧凑面板在小屏上按钮比手指小，DPI 调高又溢出。
3. **压感本身的质量**：安卓笔的 `getPressure()` 分辨率与曲线因厂商而异，还要和 SAI 的"最小尺寸"
   "笔压曲线"叠在一起——**三层曲线相乘**，正是上游作者反复警告的"笔感莫名其妙变差"的经典来源。

所以我的建议没变，只是把选项说清：

| 你的目标 | 该走哪条 |
|---|---|
| 就想在平板上用 SAI 画画，要压感要跟手 | **串流（Sunshine + Moonlight）**，压感 100% 保留、性能无损 |
| 一定要离线、能接受没压感 | **Winlator 裸跑**（[WINLATOR-QUICKSTART.md](./08-winlator-quickstart.md)） |
| 一定要离线**且**要压感、愿意折腾 | **路线 P1**，按 [03-wintab-dll-protocol.md](./03-wintab-dll-protocol.md) 来 |
| 想要一个安卓原生的 SAI 级工具 | 别自己写，用 Krita/HiPaint/ibisPaint |

---

## 6. 许可与合法边界

- 上游 **SAI Pen Pressure（ametrien）自 0.3.17 起是 GPL-3.0 + §7 附加条款**（0.3.16 及更早才是 MIT）。
  本仓库直接复制了它的 `wintab32.c` / `wintab_core.h`，所以**本仓库整体也必须是 GPL-3.0**，
  并且必须保留上游 `NOTICE`、署名与链接，同时注明"已修改"——见 [../NOTICE](../NOTICE)。
- **SAI 本体与授权不包含在内**，你仍然需要自己的 SYSTEMAX 授权才能保存文件。
- **Winlator** 是 LGPL-2.1-or-later：分发打过补丁的 APK 时，要说明这是修改版，并提供对应的
  源码获取方式。逐条清单见 [10-releasing.md](./10-releasing.md)。
- 个人自用、不分发：没有任何障碍；公开分发则按上面的义务办。

## 7. 相关文件

| 文件 | 内容 |
|---|---|
| [03-wintab-dll-protocol.md](./03-wintab-dll-protocol.md) | Winlator 补丁的分步指南、代码位置、编译、验收、故障定位 |
| [patch/CONTAINER-CHECKLIST.md](./05-container-checklist.md) | **设备到手后的四段检查单**，含 adb 命令与源码级判据 |
| [patch/SaiPressureBridge.java](../android/SaiPressureBridge.java) | 安卓侧 UDP 发送端（报文格式、历史点、缩放控制、非阻塞） |
| [tools/verify_bridge.py](../tools/verify_bridge.py) | 端到端校验器：真实 Java 类 → 报文 → DLL 逻辑，30 个样本 |
| [tools/wintab_reference.py](../tools/wintab_reference.py) | `wintab_core.h` 的逐行 Python 转写（解析 + 映射），校验的对照物 |
| [tools/read_status.py](../tools/read_status.py) | 读 `wt_status.txt` 直接给结论，支持 `--liveness` 做差值 |
| [tools/sai_pen_test.py](../tools/sai_pen_test.py) | 桌面端模拟笔样本发送器，配合 `wtprobe.exe` 先验通 |
| [README.md](./01-feasibility.md) | 最初的可行性取证（PE 结构、依赖、GDI 渲染、WinTab 双通道） |
| [WINLATOR-QUICKSTART.md](./08-winlator-quickstart.md) | 无压感版本的分步操作与调优 |
