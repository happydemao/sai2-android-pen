# 把 macOS 的压感桥接到安卓：补丁指南

> **注意**：本文描述的是 **0.3.0 之前**的手动装配流程（往容器里拷 DLL、导注册表）。
> 0.3.0 起这些都已内置在 APK，日常安装只看 [09-install-tutorial.md](./09-install-tutorial.md)。
> 保留本文是为了讲清报文格式、DLL 行为与排查手段。

配套文件：[`SaiPressureBridge.java`](../android/SaiPressureBridge.java)（安卓侧发送端）
上游：[ametrien/Paint-Tool-SAI-pen-pressure-macOS-fix](https://github.com/ametrien/Paint-Tool-SAI-pen-pressure-macOS-fix)（GPL-3.0，见 [../NOTICE](../NOTICE)）

---

## 0. 一句话

**DLL 一行都不用改。** 你要做的是在 Winlator 里造一个"假 macOS 助手"，按同样的 UDP 文本报文
往 `127.0.0.1:47800` 发笔样本。报文格式是文本，改起来比写日志还简单。

```
[修 Winlator 的 TouchpadView]          [容器内，Box64 跑]
 MotionEvent(pressure 0..1)   --UDP-->  wintab32.dll（原版）
        │  "seq p x y w h"                   │  WT_PACKET
        └─ 127.0.0.1:47800 ─────────────────>┤
                                              ▼
                                          sai2.exe（SAI 自己的 WinTab 分支）
```

---

## 1. 先做无补丁验证（10 分钟，强烈建议）

在动 Winlator 之前，先在 **Windows** 上把"桥 + 报文"验通。SAI 的 DLL 是普通 Win64 DLL，
`wtprobe.exe`（仓库自带）是它的替身，两个都能直接在 Windows 上跑。

```powershell
# 1) 从 release 或仓库取这两个文件
#    bridge/wintab32.dll   bridge/wtprobe.exe
mkdir C:\SAI2-bridge-test
copy wintab32.dll C:\SAI2-bridge-test\
copy wtprobe.exe  C:\SAI2-bridge-test\
"4095" | Out-File -Encoding ascii C:\SAI2-bridge-test\wt_pmax.txt   # 注意：DLL 读的是 C:\wt_pmax.txt
"4095" | Out-File -Encoding ascii C:\wt_pmax.txt

# 2) 跑探针（它加载 wintab32.dll 并显示收到了什么）
cd C:\SAI2-bridge-test ; .\wtprobe.exe

# 3) 另一个窗口，用本仓库的发送器打一串笔样本
python ..\tools\sai_pen_test.py --circle --pmax 4095
```

- 探针窗口里笔压/位置开始跳动 → **报文格式对了**，可以进第 2 步。
- 打不开或报 `WTInfo` 失败 → DLL 没被加载，检查它是否和 exe 同目录、位数是否 64。

> 提示：DLL 的调试日志靠环境变量打开：`set WT_DEBUG=1`，会写 `C:\wtlog.txt`，
> 里面能直接看到 `recv=` 计数和每包解析结果。这是排查"到底哪一段断了"的唯一可靠手段。

---

## 2. 放进容器（不改代码也能做的部分）

1. 把 `wintab32.dll` 复制到容器的
   `.../drive_c/windows/system32/wintab32.dll`
   （Winlator 的容器目录在 `/data/data/com.winlator/files/imagefs/...` 或你的自定义路径下）
2. 容器 **Environment Variables** 加：
   ```
   WINEDLLOVERRIDES=wintab32=n,b
   ```
   `n,b` 即 `native,builtin`：优先用我们的文件，找不到再退回 Wine 自带。
3. 写 `.../drive_c/wt_pmax.txt`，内容 `4095`（一行数字）。**DLL 只在 DLL_PROCESS_ATTACH 读一次**，
   之后改无效；它决定了 SAI 看到的压力轴上限。
4. 启动 SAI。SAI 默认就走 WinTab；装完没压感时，再去 **选项 → 数位板** 确认 API 选项（默认即 WinTab）。
5. 此时应该**仍然没有压感**（没人在发包），但 SAI 的笔不动也不该崩；DLL 会在
   `drive_c/wt_status.txt` 写心跳。看到这个文件 = DLL 已在 SAI 里加载成功。
   ```bash
   # 容器里/安卓上随时可看：
   cat .../drive_c/wt_status.txt     # v=1 build=... open=1 recv=0 posted=0 fetched=0 pmax=4095
   ```
   `open=1` 说明 SAI 已经打开了 WinTab 上下文。
   `recv=0` 说明还差第 3 步。

---

## 3. 打 Winlator 的补丁（**唯一的新工程**）

### 3.1 为什么必须改 Winlator

安卓只把 `MotionEvent`（含 `getPressure()`）交给**前台** App。Winlator 运行时前台就是 Winlator
自己，所以任何"独立小助手 App"都读不到你在 SAI 上的笔压 —— 这正是 macOS 那套方案搬不过来的
唯一原因（macOS 有全局 `CGEventTap`）。压力只能在 Winlator 的输入管线里截。

### 3.2 代码在哪

Winlator 的主仓是聚合仓，安卓 App 代码在子模块 `winlator-app`：

| 文件 | 作用 |
|---|---|
| `app/src/main/java/com/winlator/widget/TouchpadView.java` | **触摸/笔 → X 服务器指针**，补丁点就在这 |
| `app/src/main/java/com/winlator/widget/XServerView.java` | 仅 954 B 的 SurfaceView 壳，不用改 |
| `app/src/main/java/com/winlator/xserver/XServer.java` | `injectPointerMove / injectPointerMoveDelta / screenInfo` |

`TouchpadView.onTouchEvent()` 里已有两个分支：

- `event.isFromSource(InputDevice.SOURCE_MOUSE)` → 外接鼠标，直接 `injectPointerMove(transformPoint(...))`
- 否则（**触控笔/手指走这里**）→ `Finger` → `handleFingerMove()` → 要么
  `injectPointerMove(finger.x, finger.y)`（绝对模式），要么 `injectPointerMoveDelta(dx,dy)`（相对模式）

### 3.3 四处改动

```java
// ① TouchpadView 顶部
import com.winlator.sai.SaiPressureBridge;

// ② onTouchEvent() 的最前面，ACTION_DOWN 之前
if (event.getToolType(actionIndex) == MotionEvent.TOOL_TYPE_STYLUS) {
    saiPenEvent(event, actionMasked, actionIndex);
}

// ③ onTouchEvent() 结尾 return 前（手指/笔抬起时补一个压力 0，让 SAI 收笔）
//    这一步也可以省：ACTION_UP 时压力自然是 0。

// ④ 新增方法
private void saiPenEvent(MotionEvent e, int actionMasked, int index) {
    boolean down = actionMasked != MotionEvent.ACTION_UP
                && actionMasked != MotionEvent.ACTION_CANCEL
                && e.getPressure(index) > 0f;
    // 历史点：安卓会把 120–240 Hz 的笔样本合并进一个事件，不取历史点就会掉样本，
    // 快速笔画出来是折线而不是曲线。
    int hist = e.getHistorySize();
    for (int i = 0; i < hist; i++) {
        float[] pt = XForm.transformPoint(xform, e.getHistoricalX(index, i), e.getHistoricalY(index, i));
        SaiPressureBridge.onPenSample(pt[0], pt[1], e.getHistoricalPressure(index, i), down);
    }
    float[] pt = XForm.transformPoint(xform, e.getX(index), e.getY(index));
    SaiPressureBridge.onPenSample(pt[0], pt[1], e.getPressure(index), down);
}
```

启动时初始化（在 `XServer` 建好、知道容器分辨率之后，例如 `XServerView` 的构造或 Activity 的
`onCreate` 后段）：

```java
SaiPressureBridge.configure(xServer.screenInfo.width, xServer.screenInfo.height);
```

### 3.4 几个必须踩对的细节

1. **坐标系**：`XForm.transformPoint` 出来的已经是 X 服务器（容器）像素、左上原点、y 向下；
   `SaiPressureBridge` 内部翻成 WinTab 的左下原点。别自己再翻一次，否则笔画上下镜像。
2. **容器分辨率不是手机分辨率**：`configure()` 传 `xServer.screenInfo`，不是 `getScreenWidth()`。
   两者在非全屏/缩放时不同，传错会让笔画整体偏移。
3. **绝对 vs 相对模式**：Winlator 的 "Touchscreen" 输入模式走绝对定位，我们的样本也必须是绝对
   坐标（报文本来就是绝对的）。若用户把容器设成相对鼠标模式，SAI 的笔尖会和系统指针打架 ——
   文档里直接要求用 **Touchscreen 模式**。
4. **压力为 0 但接触**：某些触控笔在极轻接触时 `getPressure()` 返回 0。发送端已经兜底成 1。
5. **限速**：发送端把样本在 `pumpLoop` 里合并并 50 ms 空闲等待，UI 线程只做一次赋值，不会卡笔。
6. **无压感设备**：`getToolType()==STYLUS` 但压力恒为 1.0 的机器（部分 USI 笔）会把笔压画成
   恒定最大。可在设置里给一个 "忽略压力" 开关 —— 这是取舍，不是 bug。

### 3.4.1 坐标用 8 倍定点（减少直线抖动）

报文里的位置只取比例 `x/w`、`y/h`，所以 `x y w h` 同时乘 8 不会改变位置，
只把 Android 侧 1px 的量化误差降到 1/8px。DLL 自己的 `wtc_screen_to_sample()`
也用了同样的 8 倍约定。

0.3.10 起 `PenBridge` 发送的是：

```
"seq pressure (x*8) ((H-y)*8) (W*8) (H*8)"
```

关闭 SAI 的抖动修正后，直线上的细小台阶/抖动会明显减少；如果仍有笔硬件噪声，
下一步可以在 Android 侧加 One Euro 低通，但那会引入几毫秒延迟。

### 3.5 编译

Winlator 需要：Android SDK（compileSdk 34 左右）+ NDK + CMake，子模块要一起拉。

```bash
git clone --recursive https://github.com/brunodev85/winlator.git
git clone https://github.com/brunodev85/winlator-app.git   # 若 --recursive 没拉到
cp -r patch/SaiPressureBridge.java winlator-app/app/src/main/java/com/winlator/sai/
# 按 3.3 改 TouchpadView.java
cd winlator && ./gradlew assembleDebug          # 产物：app/build/outputs/apk/debug/*.apk
```

> 版本注意：`winlator-app` 主线在动，`TouchpadView` 的方法名可能变。补丁逻辑只有十几行，
> 对着 3.3 的描述在任何版本都能落地 —— 要找的是"拿到笔的绝对坐标、且知道容器分辨率"的那个点。

---

## 4. 验收顺序（照这个顺序，问题一眼定位）

| 检查 | 期望 | 不对说明什么 |
|---|---|---|
| `drive_c/wt_status.txt` 有 `open=1` | DLL 被 SAI 加载并开了上下文 | DLL 位置/覆盖规则/SAI 的 WinTab 开关错了 |
| `wt_status.txt` 的 `recv=` 在涨 | UDP 报文到了 | Winlator 补丁没生效，或端口/回环被挡 |
| `posted=` 在涨 | DLL 往 SAI 投了 WT_PACKET | SAI 没有 WinTab 上下文（回到第 1 行） |
| `fetched=` 在涨 | SAI 真的取包了 | SAI 在丢包，通常是 pmax 不匹配或压力恒 0 |
| 画出来线有粗细变化 | 压感通了 | 若 `fetched` 涨但粗细不变 → SAI 里笔刷的"最小尺寸"被设成 100% |

**调试开关**（容器环境变量）：`WT_DEBUG=1` → `C:\wtlog.txt`，每包都有记录，含 `gaps=`（丢包数）
和 `RATE recv= fetches=` 两个速率探针。这个日志是上游作者反复强调的第一手段，别靠猜。

---

## 5. 还差什么（诚实清单）

| 项 | 状态 |
|---|---|
| wintab32.dll 在 Box64 下能否加载 | **未实测**。它是 mingw 编的普通 x64 DLL，只用 winsock2/临界区/消息钩子，理论上没问题；先用 `wtprobe.exe` 在容器里跑一遍即可验证（不用 SAI） |
| SAI 在安卓上的笔迹延迟 | 仍在。这是 Box64 翻译 GDI 软件渲染的固有成本，压感补上也不改变 |
| 倾斜/旋转/橡皮头 | 上游就没转发（只做压力）。安卓侧 `getAxisValue(AXIS_TILT)` 有值，但要改 DLL 的 orientation 字段才用得上 |
| Winlator 各分支（Cmod/Bionic/Frost/Ludashi） | 补丁点相同，方法名各自略有差异 |
| 许可 | 上游 GPL-3.0：可以改、可以分发，但要保留 `NOTICE` 与署名、注明已修改；Winlator 是 LGPL-2.1+，分发 APK 前先看 [10-releasing.md](./10-releasing.md) |
