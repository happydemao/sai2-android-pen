# 交接提示词：SAI on Android 真压感（给新窗口用）

> 更新时间：2026-11-18 02:50。这一版**修好了"补丁版打不开"**，并且**打通了"笔点 UI"**。

## 一、目标

让 **PaintTool SAI Ver.2（Windows x64）在 Android 上运行，并用外接数位板的真实压感作画**。
最终形态：**装好 APK 就能用，不要 root、不要命令行、不要常驻外部进程**。

## 二、当前状态

| 能力 | 状态 |
|---|---|
| SAI 在 Winlator 里运行、画布正常 | ✅ 已达成 |
| 数位板压感进入 SAI（线条粗细随力度变化） | ✅ 已验证 |
| 内置发送端（编进 APK，无 root 无命令） | ✅ 已达成 |
| 用笔**点击 UI**（点菜单、工具栏、列表） | ✅ **已解决**（见第六节）；K40 的 BLE 笔与 Tab S9+ 的内置 S Pen 均已验证（见第六点五） |
| 手指操作 UI | ⚠️ 可用，双击仍偏难 |
| 倾斜 / 橡皮 | ⏸ 暂缓（协议无字段，需重编 DLL） |
| SAI 首次落笔弹 `FileError(161)` | ⚠️ errlog 仍记 3 条，但**已不再弹窗**（见第八节） |

上一版的"App 打不开"**已经修好**，当前 Release 里的 `Winlator-11.2-SAIPen.apk`（02:27 构建）是可用版本。
2026-11-18 03:00 又在原目录重装了一次 SAI，压感链路未受影响（`wt_status` 的 recv/posted 正常增长）。

## 三、环境与路径

```
工作区          <workspace>
项目目录        
设备            Redmi K40 Pro（Android 17，arm64-v8a）
adb             adb.exe
原始 APK        .stage/base.apk        （Winlator 11.2，作者签名）
调试签名        .stage/debug.keystore    （你自己的调试密钥；本仓库不含该文件）
最小 SDK        android-sdk             （platform-35 + build-tools 36）
JDK 21          tools-jdk\jdk-21.0.13.0.2+1
smali 工具      .stage/smali-tools\{baksmali,smali}-2.5.2.jar
构建脚本        build-apk.ps1            ← 一键重建
源码 PenBridge   patch\PenBridge.java
```

## 四、构建：`build-apk.ps1`（不用 Gradle）

```powershell
cd sai2-android
powershell -NoProfile -ExecutionPolicy Bypass -File .\build-apk.ps1 -Install
```

脚本做的事（可重复、每次从 `base.apk` 重新反编译，不依赖 `smali-out` 的脏状态）：

1. 从 `base.apk` 取干净 `classes.dex` → baksmali 到 `.stage\smali-clean`
2. 打 3 处 smali 补丁（见第五节）→ smali 汇编成 `classes.dex`
3. `javac --release 11` + `d8` 编 `PenBridge.java` → `classes2.dex`
4. 基于 `base.apk` 替换两个 dex → `zipalign -P 16 -f 4` → `apksigner`（**同一把 debug.keystore**）
5. 可选 `adb install -r`（同签名 → 容器数据、SAI 安装全保留）

注意点（都踩过坑）：
- `.ps1` **必须纯 ASCII**：Windows PowerShell 5.1 把无 BOM 的 UTF-8 中文注释按 ANSI 解析，会报语法错
- 函数参数别叫 `$args`（自动变量），会静默传不进去
- 删 `.stage\smali-clean` 要用长路径安全删除（`\\?\` 前缀），否则 >260 字符的 kotlin 路径删不掉
- 写 smali 时**不能带 UTF-8 BOM**，smali 2.5.2 会在第 1 行报 `required (...)+ loop did not match`

## 五、补丁结构：smali 只留 3 个调用，逻辑全在 Java

`TouchpadView.smali` 里只有：

| 位置 | 改动 |
|---|---|
| `updateXform` 开头 | `PenBridge.configure(w,h)` + 强制 `moveCursorToTouchpoint=true` |
| `onTouchEvent` 入口 | `invoke-static {p0,p1}, PenBridge->handle(View, MotionEvent)V` |
| `onExternalMouseEvent` 入口 | 同上 |

**为什么不再写 smali 分支（重要，别走回头路）**：
上一版在 `onExternalMouseEvent` 里手写了按压/抬起分支，一进容器就
`VerifyError: [0x4C] tried to get class from non-reference register v8 (type=Boolean)` → FATAL。
根因：原版该方法 `.registers 8`（p0=v6、p1=v7），上一版把 `.registers` 改成 10 之后**仍用 v8/v9 当临时寄存器**，
等于把 `this` 和 `event` 覆盖成 Boolean，校验器直接拒绝整个类。
现在补丁**不碰任何寄存器**，只用 `{p0,p1}`，所有判断都在 Java 里，异常全吞。

`PenBridge` 用**纯反射**访问 `com.winlator.*`（`xServer`/`xform`/`XForm.transformPoint`/`injectPointer*`/`Pointer$Button`），
这样编译只依赖 `android.jar`，不用维护 stub，也不会因跨 dex 引用再触发校验问题。

## 六、笔点击原理（已解决的那一半）

**硬件事实**：这支笔（`WH851-545 Pen`，BLE HID）只发 `HOVER_*`（悬停，压力恒 0）和
`ACTION_DOWN/MOVE/UP`（接触，压力 0.005~0.14），**从不发 `ACTION_BUTTON_PRESS`**，
所以 Winlator 原生逻辑永远不会按下鼠标左键 → 能画、但点不了 UI。

**判据顺序（别把压力当主判据）**：
1. 动作层：`ACTION_DOWN/MOVE` 出现 = 笔尖接触（悬停走 `HOVER_*`）
2. 压力层：压力 > 0.35 也算按下（兜底）
3. 释放：`ACTION_UP/CANCEL/HOVER_EXIT/POINTER_UP`，或压力 < 0.02
4. 看门狗：>700ms 没收到笔事件自动松开（BLE 可能不给 HOVER_EXIT，否则左键会"粘住"）

按下顺序必须是 **先 `injectPointerMove(x,y)` 再 `injectPointerButtonPress`**，反了会点在旧位置。
坐标用 `TouchpadView.xform` 矩阵变换，和 Winlator 原生一致。

## 六点五、两种笔 / 两台设备的区别（踩过的坑）

同样是"数位板"，Android 上报的 source 完全不同，补丁必须区别对待：

| 设备 | 笔 | `MotionEvent.getSource()` | Winlator 原生行为 | 我们需要做的事 |
|---|---|---|---|---|
| Redmi K40 Pro | BLE 笔 `WH851-545 Pen` | `0x2002`（含 MOUSE） | `TouchpadView.onTouchEvent` 的 `ACTION_DOWN` 第一行 `isFromSource(SOURCE_MOUSE)` 直接 return → **永远不按左键** | **必须**用压力/接触合成 press/release |
| Galaxy Tab S9+（SM-X810） | 内置 S Pen | `0x5002` = `STYLUS\|TOUCHSCREEN\|CLASS_POINTER` | 走正常触摸分支，tap 时自己 `pressPointerButtonLeft` + 30ms 后 release → **原生就会点** | **不能**再合成，否则一次点击变两声 |

判断"要不要合成"只能看 **`0x2000` 位**，不能写 `(src & SOURCE_MOUSE) != 0`：

```
SOURCE_MOUSE      = 0x2002   // 里面含 SOURCE_CLASS_POINTER(0x0002)
S Pen            = 0x5002   // 也含 0x0002
0x5002 & 0x2002  = 0x0002 ≠ 0  → 误判成"鼠标型笔" → 重复点击（"点一下响两下"）
```

正确写法（`PenBridge.needsSyntheticClick`）：

```java
return (ev.getSource() & 0x2000) != 0;   // 只认真正的 MOUSE 位
```

### 但"关掉合成"会连带丢掉拖动

原生 `handleFingerMove` 对单指**只 `injectPointerMove`，从不按左键**；只有抬起时
`Finger.isTap()`（**时长 < 200 ms 且 位移 < 10 px**，屏幕坐标）为真才 `press+release` 一次。
所以纯靠原生：能点、不能拖。

正确做法是**两种笔各用一套策略**（`PenBridge`）：

| 笔型 | 策略 | 行为 |
|---|---|---|
| 鼠标型（有 `0x2000` 位，K40 的 BLE 笔） | `syncLeftButton`：**接触即按下**，抬起松开 | 原生根本不按，全靠我们 |
| 正规 STYLUS（S Pen） | `syncDragOnly`：**位移 ≥10 px 或按住 ≥200 ms 才按下**，抬手松开 | 与 `isTap()` 严格互补：轻点归原生、拖动归我们 |

阈值必须和原生 `isTap()` 互补，否则边界上手势会既被我们按下、又被原生点一次（又是两声）：

```
原生点击：<200ms 且 <10px
我们拖动：≥10px 或 ≥200ms      ← 正好是补集
```

**现象对照**：
- 点一下 = 响两下（菜单一闪就关、按钮生效两次）→ 合成没关掉，查 `0x2000` 这一位
- 能画但点不了 UI → 合成没打开（K40 那类鼠标型笔）
- 能点但拖不动 → 拖动合成没开（正规 STYLUS 必须走 `syncDragOnly`）

## 六点六、没有 logcat 的设备怎么调

Galaxy Tab（Android 16）上 `adb logcat` 是**完全空的**（`-b all` 也没内容，logd 被关）。
排错改用 `PenBridge.flog()`：把事件流写到 app 自己的目录，`adb root` 直接读：

```powershell
adb shell su -c "tail -30 /data/data/com.winlator/files/pb.log"
adb shell su -c "grep -c SYNTH /data/data/com.winlator/files/pb.log"   # 应恒为 0（S Pen）
```

每行格式：`ev act=<action> src=<source> tool=<toolType> pr=<pressure> synth=<是否我们合成> t=<uptime>`。
`act` 含义：0=DOWN 1=UP 2=MOVE 7=HOVER_MOVE 9=HOVER_ENTER 10=HOVER_EXIT。
一次点击应该只有 **一组** `0 → 2… → 1`；出现两组就是重复注入。

## 七、设备侧布局（Winlator 11.2，Wine 10.10 x86_64 + box64 0.4.4）

> **换容器/新建容器后必须重做**：`wintab32.dll` + `wt_pmax.txt` + `user.reg` 的
> `[Software\\Wine\\DllOverrides] "wintab32"="native,builtin"`。
> 这三样都在容器内部，不跟 App 走。缺了 reg 覆盖时 Wine 会加载内置空实现，
> SAI 直接报「**WinTab 驱动返回了错误**」（2026-11-18 12:45 在 xuser-2 上复现并修复）。
>
> **2026-11-18 13:00 起已从根上解决**：`assets/container_pattern.tzst`（新容器的模板）
> 已被改造，预置了 `wintab32.dll`、`wt_pmax.txt` 和 `DllOverrides`/`WT_DEBUG` 注册表项。
> 因此**以后新建的容器天生带压感桥**；已存在的容器仍需手动补。
> 模板改造产物：`.stage\container_pattern_patched.tzst`，`build-apk.ps1` 会自动替换进 APK。

### 注册表"导入成功却没生效"的坑（重要）

Wine 的注册表改动**先进 wineserver 的内存**，只有 wineserver **正常退出**时才写回
`.wine/user.reg`。以下操作会让改动直接蒸发：

- 导入后**直接切到另一个容器**（Winlator 会结束上一个会话）
- 从最近任务**划掉 App**
- 被 `adb shell am force-stop` 或系统杀进程

**判断有没有真正落盘**：看该段的段头时间戳。写过盘的段时间戳是"现在"，没写过就还是
模板里的老值（2025-09-22 附近）。真实案例：

```
xuser-1：[Software\\Wine\\DllOverrides] 1794972604   ← 被写过（2026-11-18）
xuser-2：[Software\\Wine\\DllOverrides] 1758569987   ← 模板原值，导入从未落盘
```

**可靠做法**（任选其一）：
1. 导入后按返回键正常退出容器，等 3~5 秒，再进；
2. 容器停着时用 adb 直接改文件（`sed -i '/^\[Software..Wine..DllOverrides\]/a ...'`），
   直接落盘，不受 wineserver 影响；
3. 用新版 APK，新建容器自带，根本不用导入。

- 容器 = `home/xuser-N` + `.container`；`home/xuser` 是指向最新容器的符号链接；当前 **Container-2**
- prefix：`/data/data/com.winlator/files/rootfs/home/xuser-2/.wine`
- `drive_c` 内已就位：
  - `windows/system32/wintab32.dll`（自制）
  - `wt_pmax.txt` = `4095`
  - `SAI Ver.2/`（完整安装，含 `init/`、`sai2.ini`）
- 注册表 `.wine/user.reg`：`[Software\\Wine\\DllOverrides] "wintab32"="native,builtin"`；`[Environment] "WT_DEBUG"="1"`
- **Winlator 设置必须取消勾选 `Capture pointer on external mouse`**
- **SAI 必须选 `Other → Options → Pen Tablet → Use WinTab API`** 并重启 SAI
- 校验：`drive_c/wt_status.txt` 出现 `open=1`、`recv/posted/fetched` 增长

启动容器做自动化测试时用（免去点屏幕，`input tap` 在这台机器上被系统拦了）：

```powershell
adb shell am start -n com.winlator/.XServerDisplayActivity --ei container_id 2
# 不带 container_id 会因 getContainer() 返回 null 抛 NPE —— 这是正常现象，不是 bug
```

## 八、已知问题：SAI 每次开图第一笔画下去弹 `FileError(161)`

现象：`Discharging of history data into file took a fatal error` + `A file I/O error has occurred`，
**每个会话只弹一次**（第一笔），点掉后正常画，**撤销正常**。

SAI 自己在 `Documents/SYSTEMAX Software Development/SAIv2/error/errlog.sai2.*.txt` 里记的原因：

```
!FileError(161): Path is invalid.
  [C:\users\xuser\AppData\Local\SYSTEMAX Software Development\SAIv2\hisdata\<时间戳>.2\2.0.0.h.tmp]
SysError(0x000000A1): Path is invalid.
```

`0xA1 = 161 = ERROR_BAD_PATHNAME`。已经查清的事实（别再重复）：

- **不是权限/磁盘/内存**：磁盘 125G 空闲；`SAIv2` 目录可写（`sai2.json`、errlog 都在写）
- **不是 SELinux**：`setenforce 0`（Permissive）时 errlog 里**同样有这 3 条**，用户当时只是没看到弹窗
- **不是文件系统调用失败**：strace 显示 `.h.tmp` 的创建、`pwrite64(…, 57344)`、`fsync` **全部成功**（fsync 返回 0）
  → 失败点在一个**不落 syscall 的 Win32 路径 API**（Wine 的路径解析判定"路径语法非法"）
- 历史目录 `hisdata/<时间戳>.2` 由 SAI 自己创建/清理；`.wine` 在 f2fs 上（不是 FAT）
- 每个会话固定 3 条；只有最早那次会话（00:50，`164952`）是 0 条

结论：**SAI 与该 Wine 构建的兼容问题，非致命**。要彻底修需要改 Wine 的路径/短名处理
或换 SAI 版本，性价比低，建议先接受（弹一次、不影响作画和撤销）。

**后续验证（03:00）**：在原目录重装 SAI 后，
errlog 里**仍然每次会话记 3 条同样的 `Path is invalid`**（文件名从 `2.0.0.h.tmp` 变成 `2.0.0.a.tmp`），
但用户这次**没有看到弹窗**，画笔粗细正常。所以：错误本身还在，弹窗可能被"不再提示"或窗口遮挡过。

## 八点五、在容器里重装 / 更新 SAI（可复现流程）

> 本项目**不含 SAI**。请使用你自己合法取得的 SAI Ver.2（SYSTEMAX 官方发行版是一个
> `sai2-...-64bit.zip`，解压即用，不需要安装器）。本仓库不为第三方重打包安装器提供步骤。

### 解压版（官方发行版的常规做法）

```powershell
# 整目录放进容器，必须保留 init/ 与 sai2.ini，只放 sai2.exe 会报"构成 SAI 的文件不存在"
adb push sai2-64bit /data/local/tmp/
adb shell su -c "cp -r /data/local/tmp/sai2-64bit <prefix>/drive_c/SAI2 && chown -R 10314:10314 <prefix>/drive_c/SAI2"
```

重装或换版本**不会**动已经放好的 `wintab32.dll`、`wt_pmax.txt`，也不会动 `user.reg` 里的
`wintab32=native,builtin`——装完直接就是能压感的状态。

### 如果确实要跑一个 32 位安装器

这个 Wine 前缀是 `i386-windows + x86_64-unix`（新 WoW64），32 位 PE 安装器能跑。三个要点：

1. 先 `adb push` 到 `/data/local/tmp/`，再 `su -c cp` 进 `drive_c`，**改回 app 的 uid**
   （`stat -c %u /data/data/com.winlator` 查），否则 Wine 读不了这个文件。
2. 用 `am start` 直启，`exec_path` 必须是 **Unix 路径**，Winlator 自己转 DOS 路径：

```powershell
adb shell am force-stop com.winlator
adb shell am start -n com.winlator/.XServerDisplayActivity \
  --ei container_id 2 \
  --es exec_path /data/user/0/com.winlator/files/rootfs/home/xuser-2/.wine/drive_c/install/setup.exe
```

3. 有些机型 `adb shell input tap` 被系统拦掉（点了没反应），但**键盘事件可用**：
   `input keyevent` 里 `61`=TAB、`66`=ENTER、`19`=↑、`20`=↓、`22`=→、`23`=↓。
   Inno Setup 向导可以用 `TAB` / `ENTER` 走完；焦点停在单选控件上时 `ENTER` 不触发"下一步"，
   先 `TAB` 把焦点移开即可。

排错工具（已验证可用）：`/system/bin/strace`（toybox 版，**不支持 `-Z`/`-c` 之外的过滤技巧**，
用 `-e trace=openat,write,pwrite64,fsync,ioctl,...` 显式列 syscall；`-f -tt -y` 很好用）。
SAI 自己的错误日志比 strace 更直接，先看它。

## 九、回滚 / 恢复

```powershell
# 现在不需要卸载：补丁版与 base.apk 都可用 install -r 覆盖
adb install -r -d "dist/Winlator-11.2-SAIPen.apk"

# 真要回原版作者签名（会丢容器数据，需先备份）
adb uninstall com.winlator
adb install ".stage/base.apk"
```

用户数据备份：`/data/local/tmp/wine_prefix_backup.tgz`（221 MB，含 SAI 完整安装、DLL、注册表）。
还原（需 root）：`su -c 'cd /data/data/com.winlator/files/rootfs/home/xuser-2 && tar xzf /data/local/tmp/wine_prefix_backup.tgz'`

## 十、产物

```
dist/Winlator-11.2-SAIPen.apk   当前可用版（02:27，150.2 MB）
dist/pensender.dex              root 版发送端（app_process，兜底/对照）
dist/wintab32.dll               DLL
dist/wt_pmax.txt                内容 4095
patch\*.sh                              容器内诊断脚本（strace/errlog/ini）
07-status-and-limits.md / 06-device-test-log.md   状态与实测
history/                                        早期交接与构建记录
```
