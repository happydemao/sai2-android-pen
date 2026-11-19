# PaintTool SAI Ver.2 安卓安装教程（图文版·新手向）

> 目标：在 **不 root、不打命令** 的前提下，让 Windows 版 PaintTool SAI Ver.2 跑在 Android 上，
> 并且**外接数位板有真压感、笔能点菜单**。
>
> 适用设备：Android 10 以上、arm64、支持 OTG 或蓝牙的数位板。
> 本教程按「小米/Redmi + Winlator 11.2 + Container」的实际流程写成，其他机型同理。

---

## 目录

1. [先看这里：两种安装方式怎么选](#一先看这里两种安装方式怎么选)
2. [准备工作：文件清单](#二准备工作文件清单)
3. [第 1 步：装补丁版 Winlator](#第-1-步装补丁版-winlator)
4. [第 2 步：建容器 + 两个必改设置](#第-2-步建容器--两个必改设置)
5. [第 3 步：装中文字体（不然中文全是方块）](#第-3-步装中文字体不然中文全是方块)
6. [第 4 步：装 SAI（选中文简体）](#第-4-步装-sai选中文简体)
7. [第 5 步：装压感桥（3 个文件）](#第-5-步装压感桥3-个文件)
8. [第 6 步：SAI 里打开 WinTab](#第-6-步sai-里打开-wintab)
9. [第 7 步：验收](#第-7-步验收)
10. [日常怎么用](#日常怎么用)
11. [常见问题 FAQ](#常见问题-faq)
12. [附录：原理与文件位置](#附录原理与文件位置)

---

## 一、先看这里：两种安装方式怎么选

| 你的情况 | 用哪种方式 |
|---|---|
| 只有手机，不想碰电脑 | **方式 B（纯触屏）**：用 Winlator 自带文件管理器复制，**零 root、零命令**，本教程推荐 |
| 手边有电脑 + 会 adb，且手机**已 root**（本机是 KernelSU） | **方式 A（adb）**：最快，几分钟搞定 |

两种方式装出来**完全一样**，区别只是"文件怎么放进容器"。

> ⚠️ 方式 A 的 `adb shell su -c "cp ..."` 需要 root，因为容器目录在 App 私有空间
> （`/data/data/com.winlator/...`），**普通 adb 没有权限写入**。
> 没有 root 就老老实实用方式 B，一样能装好。

> ⚠️ **重要前提**：如果你手机上已经装了**官方原版 Winlator**，必须先卸载才能装补丁版（签名不同，无法覆盖）。
> **卸载会清空容器和里面的 SAI**。如果里面已经有画稿/配置，先备份：
> 用文件管理器把 `Android/data/com.winlator/files/rootfs/home/` 整个复制出来。
> 全新安装则无此问题。

---

## 二、准备工作：文件清单

补丁版 APK 在 [Releases](/releases) 下载；DLL、注册表片段在仓库的 `dist/` 里；SAI 本体与字体需要你自己准备：

| 文件 | 用途 | 大小 |
|---|---|---|
| `Winlator-11.2-SAIPen.apk` | 补丁版 Winlator（内置压感发送端 + 笔点击支持） | 150 MB |
| PaintTool SAI Ver.2 64bit ZIP | **你自己合法取得的副本**（SYSTEMAX 官方发布） | ~25 MB |
| `wintab32.dll` | 压感桥 DLL（我们自己写的） | 181 KB |
| `wt_pmax.txt` | 压力上限配置（内容一行 `4095`）。**不复制也能画**，但会退回内置默认 1023、压力轴只剩 1/4 | 4 B |
| `NotoSansCJK-Regular.ttc` | 中文字体（解决方块字，从设备系统字体提取） | 31 MB |
| `1-压感覆盖.reg` | 让 Wine 优先加载我们的 wintab32.dll | 326 B |
| `2-中文字体.reg` | 让 Wine 用中文字体替换 Tahoma 等 | 582 B |

**方式 A** 额外需要：电脑装好 adb（或手机装「甲壳虫ADB助手」这类工具）。

---

## 第 1 步：装补丁版 Winlator

1. 手机上允许「安装未知来源应用」。
2. 点击 `Winlator-11.2-SAIPen.apk` 安装。
3. 首次启动如果弹出 **16 KB 对齐/调试签名** 的提示，点「确定/继续」即可，不影响使用。

> 这个 APK 是**基于原版 Winlator 11.2 逐字节替换了两个 dex**做出来的：
> 资源、原生库、assets 全是原版，所以不会出现"卡在 Starting up"。
> 它只多做两件事：
> ① 把数位板压力通过 UDP 发给容器里的 `wintab32.dll`；
> ② 把"笔尖接触"翻译成鼠标左键，让笔能点菜单。

---

## 第 2 步：建容器 + 两个必改设置

### 2.1 新建容器

1. 打开 Winlator → 右上角 **＋** → 新建容器。
2. 名字随便（如 `SAI`），**屏幕分辨率选 `1280x720`**（SAI 在这个分辨率下布局最舒服，也最省性能）。
3. 其他保持默认，保存。

### 2.2 两个必改设置

| 位置 | 设置 | 为什么 |
|---|---|---|
| 容器 → ⋮ → **设置** | **取消勾选 `Capture pointer on external mouse`**（捕获外部鼠标指针） | 勾着的话容器会抢走系统光标，**数位板事件根本进不来** |
| 容器 → ⋮ → **设置** → 图形 | 建议 `Turnip + DXVK`（默认即可） | 兼容性最好 |

### 2.3 第一次进容器

点容器右侧 **▶** 启动。首次会自动初始化 Wine（可能要 1～5 分钟，屏幕可能黑一会儿），
看到 Windows 桌面/文件管理器窗口就成功了。

> 想省事：退出容器用返回键或右上角 ✕，**别直接强杀 App**，避免 Wine 写配置被中断。

---

## 第 3 步：装中文字体（不然中文全是方块）

> 这一步很多人会跳过，结果装 SAI 时满屏 `□□□`。**先装字体，后面全程中文可读。**

### 方式 A（adb，**需要 root**）

```powershell
# 1) 推字体进手机（这步不需要 root）
adb push NotoSansCJK-Regular.ttc /sdcard/Download/

# 2) 拷进容器的 C:\windows\Fonts（需要 root）
adb shell "su -c 'cp /sdcard/Download/NotoSansCJK-Regular.ttc /data/data/com.winlator/files/rootfs/home/xuser/.wine/drive_c/windows/Fonts/'"
```

> 路径里的 `home/xuser` 是 Winlator 自动维护的符号链接，**永远指向你最新的容器**，
> 所以不用管里面到底是 `xuser-1` 还是 `xuser-2`。

### 方式 B（纯触屏）

1. 用手机文件管理器把 `NotoSansCJK-Regular.ttc` 放进 **Download** 文件夹。
2. 进容器，打开文件管理器（桌面上那个窗口）。
3. 地址栏输入 `D:\`（= 手机的 Download 文件夹）→ 选中字体文件 → 复制。
4. 地址栏输入 `C:\windows\Fonts` → 粘贴。

### 两种方式都要做：导入字体替换表

1. 把 `2-中文字体.reg` 放到 **Download** 文件夹。
2. 容器里打开 `C:\windows\regedit.exe`（在文件管理器地址栏直接输这个路径就能运行）。
3. 菜单 **注册表 → 导入注册表文件** → 选 `D:\2-中文字体.reg` → 确定。
4. **重启容器**（退出再从 Winlator 点 ▶）。

做完后，安装器的中文就能正常显示了（会看到「选择安装语言 / 中文（简体）」）。

---

## 第 4 步：装入 SAI（使用你自己的合法副本）

> 本项目**不提供 SAI 本体、安装包或授权**。请自己合法取得 PaintTool SAI Ver.2
> （SYSTEMAX 官方发布的是 `sai2-...-64bit.zip`，解压即用，没有安装器）。
> 不买授权也能作画，但**保存文件**需要 SYSTEMAX 的授权证书（`.slc`）。

1. 把 SAI Ver.2 的 64 位 ZIP 传进手机，解压后**整个目录**放进容器，例如 `C:\SAI Ver.2`。
2. **必须保留 `init/` 子目录和 `sai2.ini`**——只放 `sai2.exe` 会报"构成 SAI 的文件不存在"。
3. 在容器里双击 `sai2.exe`。第一次启动 Wine 可能要等 10～20 秒才出窗口。

| 现象 | 处理 |
|---|---|
| 报"构成 SAI 的文件不存在" | 目录不完整，缺 `init/` 或 `sai2.ini` |
| 菜单、对话框全是方块 | 中文字体还没生效，回第 3 步并**重启容器** |
| 能画但"保存"不可用 | 还没有授权证书，见下 |

**授权证书（要保存文件才需要）**：SAI 里 **Others → System ID** 取本机 System ID，
到 SYSTEMAX 授权页用许可号 + 证书密码 + System ID 下载对应的 `.slc`，
放到 `sai2.exe` 同目录（新版也读 `settings/` 子目录）。以 SYSTEMAX 官方说明为准；
本项目不介入授权，也不提供任何绕过方式。

能打开画布就说明程序本身没问题（此时还没压感）。

---

## 第 5 步：装压感桥（3 个文件）

> ⚠️ **这一步是"每个容器都要做一次"的。**
> `wintab32.dll`、`wt_pmax.txt`、注册表覆盖都存放在**容器内部**，不跟着 App 走。
> 只要**新建了容器**或**换到另一个容器**，就必须把这一步重做；
> 否则 SAI 会弹「**WinTab 驱动返回了错误**」——因为 Wine 加载的是它自带的空实现。

这一步是**压感的全部秘密**：Android 收到的压力 → `wintab32.dll` → SAI。

### 5.1 放文件

| 从 | 到（容器内） |
|---|---|
| `wintab32.dll` | `C:\windows\system32\wintab32.dll` |
| `wt_pmax.txt` | `C:\wt_pmax.txt` |

> **`wt_pmax.txt` 到底有没有用？**
> 它不是"能不能跑"的开关，是**压感量程**的开关。DLL 启动时读一次；读不到就用内置默认 **1023**。
> 而手机端是按 **4095** 满量程发压力的：
>
> | Android 压力 | 换算 0–4095 | 上限 1023 时 |
> |---|---|---|
> | 0.03 | 123 | 正常 |
> | 0.15 | 614 | 正常 |
> | 0.25 | 1024 | 刚好到顶 |
> | 0.40 | 1638 | **被截断成 1023** |
>
> 所以**不复制也能画**、也不报错，你轻中力度完全正常；只有**重压不再变粗**（笔触提前到顶）。
> 想要完整压感就复制它，并且**改完要重启容器**（DLL 只在加载时读一次）。

**方式 A**（需要 root）：

```powershell
adb push wintab32.dll wt_pmax.txt /sdcard/Download/
adb shell "su -c 'cp /sdcard/Download/wintab32.dll /data/data/com.winlator/files/rootfs/home/xuser/.wine/drive_c/windows/system32/'"
adb shell "su -c 'cp /sdcard/Download/wt_pmax.txt /data/data/com.winlator/files/rootfs/home/xuser/.wine/drive_c/'"
```

**方式 B**：和上一节一样，用容器里的文件管理器从 `D:\` 复制到对应目录。

### 5.2 导入覆盖表

> ✅ **用新版 APK 的话，新建容器已经自带这 3 样了**（`wintab32.dll`、`wt_pmax.txt`、注册表覆盖），
> 本节只需给**已存在的老容器**补做。
>
> ⚠️ 若在容器**运行中**导入，改动只在 Wine 内存里；**必须正常退出容器**（返回键/✕，等几秒）才会写盘。
> 直接切容器、划掉 App、强杀进程都会让导入丢失——表现就是"我明明导入了，怎么还报驱动错误"。

1. `1-压感覆盖.reg` 放进 **Download**。
2. 容器里运行 `C:\windows\regedit.exe` → **注册表 → 导入注册表文件** → 选 `D:\1-压感覆盖.reg`。
3. **正常退出容器**（等几秒让 Wine 落盘），再重新进。

这个 `.reg` 干了两件事：

```ini
[HKEY_CURRENT_USER\Software\Wine\DllOverrides]
"wintab32"="native,builtin"     ; 让 Wine 用我们的 DLL，而不是自带的空实现

[HKEY_CURRENT_USER\Environment]
"WT_DEBUG"="1"                   ; 打开 DLL 的诊断日志
```

---

## 第 6 步：SAI 里打开 WinTab

SAI 默认用 Windows Ink，**必须手动切到 WinTab**，否则不会加载我们的 DLL。

1. 打开 SAI（容器桌面上的「SAI Ver.2」快捷方式）。
2. 菜单 **其它（Other）→ 选项（Options）**。
3. 找到 **数位板（Pen Tablet）** 页。
4. 勾选 **使用 WinTab API**。
5. 确定，然后**完全关掉 SAI 再重新打开**（不重启不生效）。

---

## 第 7 步：验收

### 7.1 看 DLL 有没有被加载

用文件管理器打开 `C:\wt_status.txt`，里面应该是：

```
open=1
recv=...      ← 你画几笔它会变大
posted=...
pmax=4095
```

- 是 `open=1` → SAI 已经连上压感桥 ✅
- 是 `open=0` 或文件不存在 → 回第 6 步，确认选了 WinTab 并重启了 SAI

### 7.2 实际画一笔

在画布上由轻到重画一条线：

- ✅ **线条粗细随力度变化** = 压感成功
- ✅ **笔能点菜单/工具栏/列表** = 笔点击成功

### 7.3 用笔点 UI（重要）

这支笔的硬件特性是：**只发悬停和"接触"，从不发鼠标按键**。
补丁版 APK 会把"笔尖接触"翻译成鼠标左键，所以你直接：

- 笔悬停 → 光标跟着走
- 笔落下 → 等于按下左键（可以拖动画选区、点菜单）
- 抬起 → 松开

**双击**：两次快速落下-抬起即可（间隔 < 300 ms）。

---

## 日常怎么用

1. 打开 Winlator → 点容器 **▶** → 等桌面 → 双击 SAI。
2. 必须记住的两条设置（改了就失效）：
   - Winlator：**不要**勾 `Capture pointer on external mouse`
   - SAI：**使用 WinTab API**
3. 建议在 SAI 里把常用快捷键设到工具栏，笔点比敲键盘方便。
4. 存盘路径建议放 `D:\`（= 手机 Download），方便用手机文件管理器拿出来。

---

## 常见问题 FAQ

### Q1：中文全是方块 / 方框

字体没装好。检查三件事：
1. `C:\windows\Fonts\NotoSansCJK-Regular.ttc` 是否存在（31 MB 那个）；
2. `2-中文字体.reg` 是否导入；
3. 导入后**有没有重启容器**。

### Q2：能画但线条粗细不变（没压感）

按顺序查（90% 是前两条）：
1. SAI 里是否选了 **使用 WinTab API**，并且**重启过 SAI**；
2. 是否导入 `1-压感覆盖.reg`（`wintab32=native,builtin`）；
3. `C:\windows\system32\wintab32.dll` 是否还在（**重装过 SAI 的话要检查**）；
4. `C:\wt_status.txt` 里 `open` 是不是 1。

### Q2.5：线条有粗细，但**重压不再变粗** / 一用力就到顶

八成是 `C:\wt_pmax.txt` 没放（DLL 退回默认 1023，量程只有 1/4）。
把内容为 `4095` 的 `wt_pmax.txt` 放进 `C:\`，**重启容器**，再画一笔对比。
判断方法：打开 `C:\wt_status.txt`，`pmax=4095` 才对，`pmax=1023` 就是这个原因。

### Q3：笔能画、但点不了菜单

装错 APK 了——必须是本文这套**补丁版**。官方原版 Winlator 没有这个合成逻辑。

### Q3.5：换了个容器后，SAI 弹「WinTab 驱动返回了错误」/ 完全没压感

**换容器 = 重做第 5 步。** 按顺序检查（都在**新容器**里）：

1. `C:\windows\system32\wintab32.dll` 存在，且大小 **185,607 字节**（小得多的就是旧的/错的）；
2. `C:\wt_pmax.txt` 存在，内容 `4095`；
3. 导入过 `1-压感覆盖.reg`（注册表里 `HKCU\Software\Wine\DllOverrides` 下有 `wintab32=native,builtin`）；
4. 重启容器后打开 `C:\wt_status.txt`，`open=1` 就成了。

有 root/adb 的话，一条命令补注册表（先退出容器）：

```powershell
adb shell su -c "sed -i '/^\[Software..Wine..DllOverrides\]/a \"wintab32\"=\"native,builtin\"' /data/data/com.winlator/files/rootfs/home/xuser/.wine/user.reg"
```

### Q4：光标消失 / 笔完全没反应

Winlator 容器设置里 `Capture pointer on external mouse` 又勾上了，取消即可。

### Q5：每次打开画第一笔，弹 `FileError(161) / A file I/O error has occurred`

**已知问题，非致命**：SAI 把"撤销历史"写盘时，Wine 返回了 `ERROR_BAD_PATHNAME`。
- 点掉对话框即可，作画、撤销、保存都正常；
- 已排除权限、磁盘、内存、SELinux（排查记录见 [04-patching-winlator.md](./04-patching-winlator.md) 第八节）；
- 要彻底修只能改 Wine，性价比低。

### Q6：装完 SAI 打不开 / 一进容器就闪退

1. 确认装的是补丁版 APK（早期版本有过 dex 校验错误，已修）；
2. 容器分辨率换成 `1280x720` 再试；
3. 换一个全新容器重装（最省时间的排查方式）。

### Q7：中文界面怎么切回英文 / 其它语言

打开 `C:\SAI Ver.2\sai2.ini`（用记事本/文件管理器打开），改第一段：

```ini
[Locale]
lang = zh      ; 中文
lang = en      ; 英文
```

改完重启 SAI。

### Q8：笔的落笔位置和光标对不上

别在 Winlator 里开"缩放/拉伸"；容器分辨率与板子比例不同会有轻微纵向拉伸，
在 SAI 里用 `Ctrl+Z` 撤销重画一笔即可，或调板子映射（高级）。

---

## 附录：原理与文件位置

### 整条链路

```
数位板 → Android MotionEvent（压力）
      → 补丁版 Winlator 里的 PenBridge（工作线程，UDP 127.0.0.1:47800）
      → 容器内 wintab32.dll（Box64 下运行）
      → WT_PACKET → SAI 画布
```

### 容器里都有什么

| 路径 | 内容 |
|---|---|
| `C:\windows\system32\wintab32.dll` | 压感桥 |
| `C:\wt_pmax.txt` | 压力上限 4095 |
| `C:\wt_status.txt` | 运行状态（`open/recv/posted/fetched`） |
| `C:\wtlog.txt` | 详细日志（排错用） |
| `C:\SAI Ver.2\` | SAI 本体、`init/`、`sai2.ini` |
| `C:\windows\Fonts\NotoSansCJK-Regular.ttc` | 中文字体 |

容器在手机里的真实位置：

```
/data/data/com.winlator/files/rootfs/home/xuser-N/
```

`home/xuser` 是指向**最新容器**的符号链接，所以路径里写 `xuser` 永远是对的。

### 想备份整套环境

把 `home/xuser-N/` 整个目录复制走即可（含 SAI、DLL、注册表、字体）。
换机/重装后放回同一位置，再建容器指向它就行。

---

*文档版本：2026-11-18。机器相关的一次性排查记录见 [04-patching-winlator.md](./04-patching-winlator.md)、[06-device-test-log.md](./06-device-test-log.md)。*
