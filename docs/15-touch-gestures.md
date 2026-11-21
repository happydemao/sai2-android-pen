# 触控屏手势：撤销 / 重做 / 平移 / 缩放 / 旋转 / 适合窗口（0.3.8）

> 目标：在不改 SAI 的前提下，把平板/触屏上最常用的两指、三指手势映射成 SAI 的
> 现有快捷键和鼠标操作。全部在 Android 侧完成，SAI 收到的仍然是普通的键盘/鼠标事件。

## 映射

| 手势 | 注入给 SAI 的操作 | 说明 |
|---|---|---|
| 两指轻点 | `Ctrl+Z` | 撤销；弹 Toast「撤销 (Ctrl+Z)」 |
| 三指轻点 | `Ctrl+Y` | 重做；弹 Toast「重做 (Ctrl+Y)」 |
| 两指平移 | `Space` 按下 + 左键按下 + 拖动 | 平移画布 |
| 两指捏合 / 张开 | `PageUp` / `PageDown` | 以光标为中心缩放画布 |
| 两指旋转 | `Shift+PageUp` / `Shift+PageDown` | 每次约 11.3°，方向跟随手指 |
| 快速向内捏合 | `Ctrl+0` | 适合窗口 / 居中全屏；弹 Toast「适合窗口 / 居中全屏 (Ctrl+0)」 |

缩放/旋转的键位是在 SM-X810 + `SAI Ver.2 2026-10-19` 上实测后定的：

- `PageUp` / `PageDown`：SAI 默认「以光标位置为中心放大/缩小」，稳定可用；
- `Ctrl+'-'` 能缩小，但 `Ctrl+'+'`、`Ctrl+Shift+'='`、`Ctrl+KP_ADD` 在这个构建里都不放大；
- 旋转用 `Shift+PageUp` / `Shift+PageDown`（SAI 默认旋转视图快捷键，每次 11.3°）；
  原来的 `Alt+Space+左键拖动` 在 Wine 下会被系统菜单干扰，实测不转。

如果 SAI 里改过快捷键，需要把 [PenBridge.java](../android/PenBridge.java)
里 `shortcutZoom()` / `rotateStep()` / `shortcutUndo()` / `shortcutRedo()` /
`shortcutFit()` 的键码改成用户自己的绑定。

## 为什么要在 smali 里消费多指事件

Winlator 原生 `TouchpadView` 对两指有自己的处理：

- 两指上下移动 → 注入 X11 滚轮 `button 4/5`（在 SAI 里会滚动视图，不是缩放）；
- 两指轻点 → 右键；
- 两指距离够大时还可能补左键按下。

这些行为和我们要做的平移/缩放/旋转直接冲突。所以 `onTouchEvent` 入口改成调用
`PenBridge.handleTouch(...)`；只要识别出多指手势，就 `return true`，原生逻辑完全
不执行。单指和笔的事件仍然返回 `false`，原行为不变。

原生在第一个手指 `ACTION_DOWN` 时已经把手指记进了 `fingers[]`。我们消费第二个手指
的 `POINTER_DOWN` 后，原生不会更新这个数组，所以 `PenBridge` 在识别出手势时用反射
清空 `fingers[]`、`numFingers`、`scrolling`，等价于「原生从未见过这次触摸」；否则
手势结束后残留的单指状态还会继续移动光标。

另外，三指轻点在真实设备上是**依次抬手**的：第一指抬起触发 `Ctrl+Y` 后，剩下两指
还在屏幕上。如果这时重新开始识别，就会被当成一次新的两指轻点，紧接着触发 `Ctrl+Z`，
表现为「重做后又撤销」。所以一次手势完成后会置 `consumeRest`，把剩余手指的
`POINTER_UP/UP` 全部消费掉，直到新的 `ACTION_DOWN` 才重新开始识别。

## 手指落笔延迟

旧版单指 `ACTION_DOWN` 就立刻发压感样本。两指轻点时，第一根手指会在 SAI 里先落一个
点；随后 `Ctrl+Z` 撤销的是这个点，而不是用户想撤销的那一笔。

现在手指要移动超过 `FINGER_DRAW_PX = 8px` 才发第一包样本：

- 单指拖动画线：从越过 8px 的位置开始画，肉眼几乎无感；
- 单指轻点 UI：不再产生一个多余的点；
- 两指/三指手势：第一根手指的样本不会发出。

如果第一根手指已经在画线，再加入第二根手指，会先正常收笔，然后开始手势。

## 识别参数

在 `PenBridge.TouchGestures` 里：

| 常量 | 默认值 | 作用 |
|---|---|---|
| `TAP_SLOP` | 16 px | 轻点允许的最大位移 |
| `TAP_MS` | 380 ms | 轻点允许的最长时间 |
| `FIT_RATIO` | 0.62 | 两指距离缩到 62% 以下才算「向内快捏」 |
| `FIT_MS` | 280 ms | 快捏的时间窗口 |
| `FIT_SLOP` | 60 px | 快捏时重心允许的漂移 |
| `FIT_SPEED` | 0.0011 | 收缩速度下限（比例/毫秒） |
| 拟合冷却 | 600 ms | 两次「适合窗口」之间至少间隔 600ms |
| `ZOOM_STEP_PX` | 55 px | 累计两指距离变化多少像素注入一次缩放 |
| `ROTATE_STEP_RAD` | 0.197 rad | 约 11.3°，对应一次 SAI 旋转快捷键 |

两指手势会先算三个分数：平移（重心位移 / 40px）、缩放（距离变化比例 / 8%）、
旋转（角度变化 / 8°）。平移必须同时满足：

- 重心位移至少 48px（`panScore >= 1.2`）；
- 平移分数不低于旋转/缩放分数的 80%（即平移必须明显占优）。

旋转或缩放只要过阈值、且不被平移大幅压过，就优先判为旋转/缩放。
这样两指旋转时即使重心有点漂移，也不会被误判成平移。

## 限制

- 两指轻点不再触发右键（右键功能在 SAI 里用不到；需要时可改成 `Ctrl+点击`）。
- 三指移动没有映射，会被忽略。
- 手势是全局的：在 SAI 的图层/颜色面板上也会生效，和滚轮缩放的限制相同。
- 手势注入的是 SAI 默认快捷键；如果用户改过 SAI 快捷键，需要同步改这里的键码。
- Toast 只在撤销、重做、适合窗口三个一次性动作上弹；平移/缩放/旋转不弹。


## 真机实测（0.3.8）

设备：Galaxy Tab S9+（SM-X810），SAI 2（2026-10-19 构建），Winlator 11.2 容器。
多指事件用 `/dev/input/event11` (sec_touchscreen) 的 MT-B `sendevent` 注入，结果：

- 两指轻点：SAI 撤销一步，Toast「撤销 (Ctrl+Z)」；
- 三指轻点：SAI 重做一步，Toast「重做 (Ctrl+Y)」；
- 两指张开：50% → 300%（两指距离约 2 倍；`ZOOM_STEP_PX=55`，单个 MOVE 最多 3 级）；
- 两指旋转：两指转 30°，画布 0° → 11.3°（一次 11.3° 步进，跟随旋转方向）；
- 两指平移：重心移动约 200 原始像素，画布跟随平移；不再抢旋转；
- 快速向内捏合：弹 Toast「适合窗口 / 居中全屏 (Ctrl+0)」，视图回到 63.9%。
  阈值已从「72%/320ms」收紧到「62%/280ms + 速度下限 + 600ms 冷却」，
  普通慢速缩小不会误触发。

## 构建

`android/PenBridge.java` 已包含全部手势逻辑；`android/build-apk.ps1` 的
`B onTouchEvent` 补丁已改为 `handleTouch(...)Z` 并在识别出手势时提前返回。
按原流程重建即可：

```powershell
.\build-apk.ps1 -BaseApk .\in\base.apk -SmaliTools .\in\smali-tools -Sdk C:\Android\Sdk -Jdk C:\jdk-21 -Install
```

## 验证

1. 单指拖动画线、单指点 UI 正常（落笔延迟 8px）。
2. 两指轻点：`pb.log` 出现 `GEST undo ctrl+z`，SAI 撤销一步，并弹 Toast。
3. 三指轻点：`pb.log` 出现 `GEST redo ctrl+y`，SAI 重做一步，并弹 Toast。
4. 两指平移：画布跟随两指重心移动，没有原生滚轮滚动。
5. 两指捏合：画布缩放，方向为「张开放大、捏合缩小」。
6. 两指旋转：画布跟随两指夹角旋转。
7. 快速向内捏合：SAI 适合窗口/居中全屏，并弹 Toast；普通慢速捏合仍然是缩放。

诊断日志：`/data/data/com.winlator/files/pb.log`，搜 `GEST`。

## 本地构建产物

`sai2-android-gh/Winlator-11.2-SAIPen-0.3.7-gestures.apk` 是在 macOS 上用
0.3.6 APK + 本目录 `PenBridge.java` / smali 补丁重新打包、并用 `keys/debug.keystore`
签名的测试包（与 0.3.6 同签名，可直接覆盖安装）。**尚未在真机上验证**；
正式发布请按 `android/build-apk.ps1` 从原始 `base.apk` 重建。

