# 蓝牙键盘在容器里完全没反应：根因与修复（2026-11-20）

> 现象：USB 键盘插上就能在 SAI / 容器里打字，换成蓝牙键盘（实测 **MCHOSE G75 V2 5.0**）
> 一个键都进不去。Android 系统里、Winlator 主界面里打字都正常，唯独进容器后失效。

## 结论先说

不是 Winlator 的输入法/焦点问题，也不是本项目的压感桥冲突，而是
`com.winlator.inputcontrols.ExternalController.isGameController()` 把**复合 BLE 键盘误判成手柄**，
于是 `xserver.Keyboard.onKeyEvent()` 在读到键码之前就 `return false`，按键被整体丢弃。

```
Android KeyEvent (device 29, MCHOSE G75 V2 5.0 Keyboard)
        │
        ▼
XServerDisplayActivity.dispatchKeyEvent()
        │  xServer.keyboard.onKeyEvent(event)
        ▼
Keyboard.onKeyEvent()
        │  if (ExternalController.isGameController(event.getDevice())) return false;   ← 这里返回了
        ▼
     (什么都没发生)
```

## 证据

同一台 Galaxy Tab S9+（SM-X810）上抓到的输入设备：

```
Device 29: MCHOSE G75 V2 5.0  Keyboard
  Sources: KEYBOARD | DPAD | MOUSE | JOYSTICK     ← 0x1002313
  KeyboardType: 2                                  ← KEYBOARD_TYPE_ALPHABETIC
  IsVirtualDevice: false
  KeyLayoutFile: /system/usr/keylayout/Generic.kl
```

原版判定逻辑只看向 `getSources()`：

```java
return (sources & 1025) == 1025 || (sources & 16777232) == 16777232;
//                                                   ^^^^^^^^^
//                                    16777232 = SOURCE_JOYSTICK | SOURCE_DPAD
```

这把键盘为了上报多媒体/鼠标/摇杆报告，同时声明了 `JOYSTICK|DPAD`，正好命中第二个条件。
USB 键盘通常只有 `KEYBOARD`（可能带 `DPAD`），不会命中，所以出现"USB 行、蓝牙不行"。

在真机上用反射直接调用安装包里的方法，对比修复前后：

```
# 原版 classes.dex
id=29 name=MCHOSE G75 V2 5.0  Keyboard src=0x1002313 kbdType=2 virtual=false
  isGameController=true

# 修复后 classes.dex
id=29 name=MCHOSE G75 V2 5.0  Keyboard src=0x1002313 kbdType=2 virtual=false
  isGameController=false
```

## 修复

只改一个方法：`ExternalController.isGameController()`。在空值检查之后、原有 `getName()` 之前，
如果 `InputDevice.getKeyboardType() == KEYBOARD_TYPE_ALPHABETIC (2)`，直接返回 `false`。

```smali
:cond_0
    invoke-virtual {p0}, Landroid/view/InputDevice;->getKeyboardType()I
    move-result v3
    const/4 v4, 0x2
    if-ne v3, v4, :cond_not_alphabetic_keyboard
    return v0
:cond_not_alphabetic_keyboard
    invoke-virtual {p0}, Landroid/view/InputDevice;->getName()Ljava/lang/String;
```

为什么用 `getKeyboardType()`：

- 这是 Android 根据键盘布局算出来的属性，全键盘是 `ALPHABETIC(2)`，游戏手柄是 `NONE(0)` /
  `NON_ALPHABETIC(1)`；不受厂商塞进来的 `JOYSTICK` 报告影响。
- 只影响"带全字母键盘的设备"，普通手柄（包括带按键的手柄）判定不变。
- 同时修好两处调用：`Keyboard.onKeyEvent()` 的提前返回，以及
  `XServerDisplayActivity.dispatchKeyEvent()` 里 `!isGameController(...) && super.dispatchKeyEvent(event)`
  的兜底分支。

## 构建与安装

`android/build-apk.ps1` 已加入同一个 smali patch（步骤 `3b`）。重新构建后：

```powershell
.\build-apk.ps1 -BaseApk .\in\base.apk -SmaliTools .\in\smali-tools -Sdk C:\Android\Sdk -Jdk C:\jdk-21 -Install
```

本次在 macOS 上的临时构建流程（只替换 `classes.dex`，保留原资源与 `.so`）：

1. `apktool d` 解包 → 改 `smali/com/winlator/inputcontrols/ExternalController.smali`
2. `apktool b --no-res` 重新汇编 dex，只取 `classes.dex`
3. 用原始 APK 的 `classes2.dex`（PenBridge）不变，回填 `classes.dex`
4. `zipalign -P 16 -f 4` → `apksigner sign`（与已安装版本同一 debug 证书，`adb install -r` 保数据）

签名证书必须与设备上已安装的一致，否则只能卸载重装（容器数据会一起没）。

## 复现/验证命令

```bash
# 看设备源与键盘类型
adb shell dumpsys input | grep -A 12 "MCHOSE G75"

# 用 app 自己的 dex 在设备上验证判定（app 是 debuggable）
adb shell run-as com.winlator sh -c \
  'CLASSPATH=/data/local/tmp/igc.dex:/data/local/tmp/orig-winlator.dex \
   app_process /system/bin TestIGC'
```

`TestIGC` 只做一件事：遍历 `InputDevice.getDeviceIds()`，对名字含 `MCHOSE G75` 的设备调用
`ExternalController.isGameController()` 并打印结果。

## 还没覆盖的

- 多媒体键（音量/播放）本来就不在 `Keyboard.keycodeMap` 里，这次没加。
- BT 键盘自带的鼠标/摇杆部分仍按 `isGameController` 判定；它不是全字母键盘，行为不变。
- 如果某款蓝牙键盘的 `getKeyboardType()` 被厂商报成 `NONE`，仍可能复现；可改用
  `event.getSource() == SOURCE_KEYBOARD` 的二次判定。
