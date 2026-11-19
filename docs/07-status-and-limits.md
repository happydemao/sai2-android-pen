# SAI 安卓压感：最终可用形态（2026-11-18 01:30）

> **2026-11-18 02:50 更新（本次）**
>
> - 修好"补丁版打不开"：根因是 `onExternalMouseEvent` 补丁把 `.registers` 从 8 改到 10 后仍用 v8/v9，
>   踩掉了 `this`/`event`，导致 `VerifyError` → 进容器 FATAL。现改为 smali 只留 3 个调用、逻辑全在 Java。
> - **笔点击 UI 已打通**：按"动作类型优先 + 压力兜底 + 700ms 看门狗"合成左键按下/抬起，任务栏、菜单、工具栏都能点。
> - 新增 `build-apk.ps1`：一键从 `base.apk` 重建并安装（同签名，容器数据保留）。
> - 已知未解决：SAI 每会话第一笔弹 `FileError(161) ERROR_BAD_PATHNAME`（非致命，撤销/作画正常）。
>   已排除权限/磁盘/内存/SELinux，syscall 层全成功，属 SAI×Wine 兼容问题。详见 [04-patching-winlator.md](./04-patching-winlator.md) 第八节。
>
> **2026-11-18 03:00**：又在原目录重装了一次 SAI，`wintab32.dll` / `wt_pmax.txt` / `user.reg` 覆盖
> 全部保留，装完压感正常（recv=1760 posted=1589）。errlog 里仍记 3 条 `Path is invalid`，但这次没弹窗。
> 重装流程与键盘导航方法（`input keyevent`，因为 `input tap` 被系统拦）见
> [04-patching-winlator.md](./04-patching-winlator.md) 第八点五节。
>
> **2026-11-18 11:55（Galaxy Tab S9+ / SM-X810 内置 S Pen）**
> 换设备后出现"点一下响两下"：根因是 `needsSyntheticClick` 用了 `(src & SOURCE_MOUSE) != 0`，
> 而 `SOURCE_MOUSE(0x2002)` 含 `SOURCE_CLASS_POINTER(0x0002)`，S Pen 的 `0x5002` 同样带这一位 →
> 误判成"鼠标型笔"，在原生已经点击的基础上又合成一次。改成只认 `0x2000` 位后修复。
> 该设备 `logcat` 完全不可用，排错改看 `/data/data/com.winlator/files/pb.log`。
>
> **2026-11-18 12:05**：关掉合成后"拖动"消失了（原生单指 MOVE 从不按左键，只有 `isTap()`
> 即 <200ms 且 <10px 才点一次）。已改成两种笔分开处理：鼠标型笔接触即按下；
> 正规 STYLUS 只在**位移 ≥10px 或按住 ≥200ms** 时按下——与 `isTap()` 严格互补，
> 于是「轻点=原生点一次、拖动=我们按下保持」，两头都保住。实测点击、拖动、压感均正常。

## 结论：已经跑通，且满足"无 root、无命令"

外接数位板 → Winlator（已打补丁的 APK 内置发送端）→ UDP → `wintab32.dll` → SAI

实测证据：

```
status:  open=1  recv=1536  posted=1514  fetched=1513
WTPacket ... press=1666 lat=1ms     ← 落笔时的真实压力
WTPacket ... press=0    lat=1ms     ← 抬笔/悬停
```

画布上出现**粗细随力度变化的笔画**（截图 `sai2-android/.shots/final.png`）。

## 交付物

`Winlator-11.2-SAIPen.apk`（150.2 MB，见 GitHub Releases）

这个 APK：
- 基于**你自己设备里那个原版 APK** 逐字节复制，只替换了 `classes.dex` 和新增 `classes2.dex`
- 资源表、原生库（35 个 `.so`）、assets 全是原版字节 → 不会出现"卡在 Starting up"
- 包名仍是 `com.winlator`，用自签名证书（换签名必须卸载重装）

## 需要用户做的唯一设置

**Winlator → Settings → 取消勾选 `Capture pointer on external mouse`**

原因：勾选时容器会 `requestPointerCapture()`，Android 收走系统光标、事件改走相对位移，
笔的事件根本到不了监听点。取消后光标跟随笔移动，事件走 `onExternalMouseEvent`。

## 补丁做了什么（4 处，全在 smali 层）

| 位置 | 改动 | 作用 |
|---|---|---|
| `TouchpadView.onTouchEvent` 入口 | 采样 → `PenBridge.onTouch()` | 手指/触屏路径的压感 |
| `TouchpadView.onExternalMouseEvent` 入口 | 采样 → `PenBridge.onTouch()` | **外接笔走的就是这条路** |
| `TouchpadView.updateXform` | `PenBridge.configure(w,h)` | 告诉桥容器分辨率 |
| `TouchpadView.onTouchEvent` ACTION_DOWN | 立即 `pressPointerButtonLeft` | 修"拖动画不出线" |
| `releasePointerButtonLeft` | 30ms 延迟 → 0ms | 修"双击被吞成单击" |

`onExternalMouseEvent` 里还按压力合成按下/抬起（**现在的规则是「有压力就落笔」**：`PRESS_STRONG = 0.0f`、`PRESS_MIN = 0.001f`）：
数位板只给 `HOVER_MOVE`，不给 `DOWN`，所以必须用压力自己判断落笔。

## 内置桥（`PenBridge`，classes2.dex）

- 报文：`"seq pressure x y w h"`，UDP → `127.0.0.1:47800`，`w,h` 为容器分辨率
- 发送在**独立工作线程**（主线程发网络会抛 `NetworkOnMainThreadException`）
- UI 线程只写"最新样本"槽位，不阻塞
- 所有异常吞掉并降级为"没有压感"，绝不让 Winlator 崩
- 只依赖 APK 本来就有的 `INTERNET` 权限

## APK 0.3.0：全集成（除 SAI 外零手工）

| 内容 | 集成方式 | 新建容器后 |
|---|---|---|
| `wintab32.dll`（185,607 B） | App 内置 base64，`PenBridge.configure()` 时自愈写入 | ✅ 自动 |
| `wt_pmax.txt` | 容器模板 `assets/container_pattern.tzst` | ✅ 自动 |
| `DllOverrides` / `WT_DEBUG` | 同上（模板 `user.reg`） | ✅ 自动 |
| 字体 + `Fonts\Replacements` | 同上（模板内 `Fonts/NotoSansCJK-Regular.ttc`） | ✅ 自动 |
| **SAI 本体** | **不集成**（商业软件，需用户自备授权） | 需自己装 |

> 为什么 DLL 不能只靠模板：Wine 的 `wineboot` 在容器首次启动时会把 builtin DLL
> 拷进 `system32`，覆盖模板里预置的那份。详见 [04](./04-patching-winlator.md) 第七节。

## 已知限制 / 后续可优化

1. **触发阈值现在是「有压力就落笔」**（v0.1.1 起）。`PenBridge` 里两个常量：
   `PRESS_STRONG = 0.0f`（按下判据，`pressure > 0`）、`PRESS_MIN = 0.001f`（抬起判据）；
   另外 `syncDragOnly` 里 ACTION_DOWN 一有压力就按下，不再等 10px / 200ms。
   如果哪天出现「悬停微压就误落笔」，把 `PRESS_STRONG` 调到 `0.005f` 左右即可；
   改的就是 `android/PenBridge.java` 里那一行，重新构建见 `docs/04`。
2. **坐标映射**：数位板整面 → 容器整面直接拉伸。板子是 16:10、容器 16:9，会有轻微纵向拉伸。
   要精确可加 letterbox（保持比例 + 居中）。
3. **倾斜/橡皮**：`ABS_TILT_X/Y`、`BTN_TOOL_RUBBER` 在 evdev 里有，但 `MotionEvent` 侧目前没转发。
4. **16 KB 对齐告警**：debug 来源的包，首次启动系统会弹一次提示，点"确定"即可。
5. 容器内已装好：`wintab32.dll`、`wt_pmax.txt`(4095)、`user.reg` 的 `wintab32=native,builtin`、完整 SAI（含 `init/`）。

## 备用方案（若以后换设备/重装）

- 不需要重编：DLL 与 `wt_pmax.txt` 放回 `drive_c` 即可
- 若某台设备的笔完全不派发（如无显示关联的 HID），仍可用 root 版 `pensender.dex` + `app_process` 兜底
- 相关脚本与记录见 [android/](../android/)、[06-device-test-log.md](./06-device-test-log.md)
