# 系统鼠标光标跟随内置笔（0.3.6）

> 现象：S Pen 悬停/落笔时，SAI 的画笔圈已经跟着笔尖走，但 Winlator 的系统鼠标光标
> 还停在旧位置；两者不同步。

## 原因

光标位置由 X server 的 `pointer` 决定，Winlator 只在两条路径里移动它：

- `TouchpadView.handleFingerMove()`：手指/触摸的 `ACTION_MOVE`，且 `moveCursorToTouchpoint`
  为 true 时；
- `PenBridge.syncDragOnly()`：正规 STYLUS 只有**已经进入拖动/长按**后才 `injectPointerMove`，
  悬停（`ACTION_HOVER_MOVE`）完全不移动。

SAI 的画笔圈来自 WinTab 样本流（`PenBridge.emit`），悬停也一直在发；所以出现
"圈跟着笔、系统光标不动"。

## 修复

在 `PenBridge.handle()` 里，对**每一个笔事件**（`isPen(ev)` 为 true）调用：

```java
moveCursorTo(pad, x, y);   // xServer.injectPointerMove(x, y)
```

位置用的是已经过 `XForm` 的容器坐标（和 WinTab 样本同一套坐标），覆盖：

- `ACTION_HOVER_ENTER` / `ACTION_HOVER_MOVE` / `ACTION_HOVER_EXIT`
- `ACTION_DOWN` / `ACTION_MOVE` / `ACTION_UP`

`syncDragOnly` 原来的"拖动时移动"保留，重复移动是幂等的。

## 验证

- S Pen 悬停在画布上时，系统光标和 SAI 画笔圈在同一位置；
- 落笔/拖动时光标继续跟随，不影响笔画和点击；
- `pb.log` 中 `act=7/9/10`（悬停）与 `act=0/2`（接触）都会走 `moveCursorTo`。

## 注意

- 只对 `isPen()` 识别出的笔生效；手指路径仍由 Winlator 原生逻辑处理。
- 如果 SAI 自己把 Windows 光标隐藏（`ShowCursor(FALSE)`），移动光标也不会让它显示；
  这种情况下只能看到 SAI 自绘的画笔圈，属于 SAI 的行为。
- 鼠标型笔（BLE，`SOURCE_MOUSE`）本来就在 `syncLeftButton` 里移动光标，这里是重复调用，无害。

## 构建

`android/PenBridge.java` 已更新，重新编译 `classes2.dex` 即可；`classes.dex` 不需要改。
产物：`Winlator-11.2-SAIPen-0.3.6.apk`。

## 补充：不强制手指移动光标；虚拟鼠标按钮单独处理（0.3.9）

曾尝试让 `setMoveCursorToTouchpoint()` 恒为 `true`，结果手指画线会“连笔”，已撤回。
现在保持 Winlator 原有设置：普通手指触摸不强制移动系统光标。

真正需要移动光标的是 **InputControlsView 的虚拟鼠标按钮**：它只注入
`injectPointerButtonPress/Release`，不会移动 X 指针，所以光标不在按钮上时，
点击会落在旧位置，表现为“虚拟按钮点不动”。

修复方式：

- `ControlElement.handleTouchDown()` 的 BUTTON 分支先调用
  `PenBridge.moveCursorForVirtualButton(element, x, y)`；
- 该方法只在绑定里含 `MOUSE_LEFT/MIDDLE/RIGHT/SCROLL` 时生效，
  用 `TouchpadView.xform` 把手指坐标转成容器坐标，再 `injectPointerMove`；
- Ctrl/Shift 等键盘按钮不会移动光标。

对应 smali 补丁在 `build-apk.ps1` 的 `control element mouse cursor` 步骤。
