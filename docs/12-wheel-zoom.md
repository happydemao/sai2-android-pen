# 鼠标滚轮：从"上下移动画布"改成"以光标为中心缩放"（0.3.5）

> 现象：在 SAI 里滚鼠标滚轮，画布会上下移动；用户期望的是像桌面 SAI 一样放大/缩小。

## 结论

这是 Winlator 侧的输入映射问题，不需要改 SAI：

- Winlator 收到 Android 的 `ACTION_SCROLL` 后，原样注入 X11 `button 4/5`（`BUTTON_SCROLL_UP/DOWN`）。
- 在 Wine + 这个 SAI 构建里，SAI 把这条滚轮消息执行成了"滚动视图"，而不是 SAI 自己
  "以光标为中心缩放"的滚轮行为。
- 与其去猜 Wine 的滚轮消息路径，不如直接用 SAI 已经验证过的键盘缩放快捷键：
  **PageUp = 以光标为中心放大，PageDown = 以光标为中心缩小**。

修复后：

```
Android ACTION_SCROLL（物理鼠标滚轮）
        │
        ▼
TouchpadView.onExternalMouseEvent()
        │  不再注入 button 4/5
        ▼
PenBridge.zoomIn() / zoomOut()
        │  反射调用 xServer.injectKeyPress/Release(XKeycode.KEY_PRIOR/KEY_NEXT)
        ▼
SAI：PageUp/PageDown → 缩放画布
```

## 改动

1. `android/PenBridge.java` 新增 `zoomIn(View)` / `zoomOut(View)`：
   - 复用已有的 `fXServer` 反射；
   - 反射取 `XKeycode.KEY_PRIOR` / `KEY_NEXT`；
   - 调 `XServer.injectKeyPress` + `injectKeyRelease`；
   - 第一次成功时写一条 `wheel -> PageUp/PageDown (SAI zoom)` 到 `pb.log`。
2. `TouchpadView.onExternalMouseEvent()` 的 `ACTION_SCROLL` 两个分支：
   - `scrollY <= -1` → `PenBridge.zoomOut(this)`
   - `scrollY >= 1` → `PenBridge.zoomIn(this)`
3. `android/build-apk.ps1` 已加入同一 smali patch（步骤 `E`）。

**只改物理鼠标滚轮**。双指触摸板滚动（`handleFingerMove`）保持原样，仍用于平移画布。

## 实测

设备：Galaxy Tab S9+（SM-X810），SAI 2（2026-10-19 构建）。

```text
滚轮向下：75% -> 50%     （PageDown）
滚轮向上：50% -> 75%     （PageUp）
pb.log: ev act=8 src=0x2002 ...
        wheel zoom out
```

测试时用 `adb shell input mouse scroll <x> <y> --axis VSCROLL,±1` 注入，等价于物理滚轮。

## 注意

- 这个映射是全局的：滚轮在 SAI 的图层/颜色面板上也会缩放画布，而不是滚动面板。
  如果以后想只在画布区域生效，可以在 `PenBridge.zoomIn/Out` 里判断指针是否落在画布窗口。
- SAI 的 PageUp/PageDown 快捷键若被用户改过，需要恢复默认，或把这里的键码换成用户自定义键。
- 如果 SAI 自己打开了"反转滚轮方向"（`OptPrfVwCtl_WheelInv`），这里的上下方向可能与之一致或相反；
  当前按"滚轮上 = 放大"实现。

## 构建

```powershell
.\build-apk.ps1 -BaseApk .\in\base.apk -SmaliTools .\in\smali-tools -Sdk C:\Android\Sdk -Jdk C:\jdk-21 -Install
```

构建产物：`Winlator-11.2-SAIPen-0.3.5.apk`。同签名覆盖安装，容器数据保留。
