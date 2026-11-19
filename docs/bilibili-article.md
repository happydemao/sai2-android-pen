# 安卓终于可以用sai2力 教程第二版

PaintTool SAI Ver.2 安卓安装教程

适用设备：Android 10 以上、arm64，支持 OTG / 蓝牙数位板，或平板自带压感笔（如三星 S Pen）。
预计耗时：20～40 分钟（大头是容器初始化和 SAI 首次启动）。

> **重要更新（0.3.0 起）**：压感桥和中文字体已经内置在 APK 里。
> 新建的容器天生带 `wintab32.dll`、压力上限、注册表覆盖和中文字体。
> 你只需要：**装 APK → 建容器 → 装 SAI**，不用再往容器里拷任何文件、也不用导注册表。

目录

- 开始之前
- 准备文件
- 第 1 步：装补丁版 Winlator
- 第 2 步：建容器
- 第 3 步：装入 SAI
- 第 4 步：验收
- 日常怎么用
- 常见问题
- 附录：旧版 APK / 老容器怎么补

## 一、开始之前

### 1.1 说明

- 这一版还不稳定，仅供尝鲜。请随时保存（Ctrl+S），别把正在画的稿子只留在内存里。
- SAI 要你自己买、自己下。官方地址：https://www.systemax.jp/ 。本项目不提供 SAI 本体、安装包或授权。
- 不买授权也能画，但保存文件需要 SYSTEMAX 的授权证书（.slc）。想先试手感的话，画完截图也行。
- 手机的下载目录在winlator中默认是D盘，可以把sai2复制到D盘中安装。

社区里流传的汉化版 / 整合包不是本项目提供的，来源和授权都没法核实，用不用由你自己判断，出问题也别指望这里能修。

### 1.2 已经装了官方 Winlator 怎么办

必须先卸载官方版——补丁版包名相同但签名不同，装不上去。

⚠️ 卸载会清空所有容器，里面的 SAI、画稿、配置一起没。如果里面有东西，先用文件管理器把
`Android/data/com.winlator/files/rootfs/home/` 整个复制出来备份。全新安装没有这个问题。

## 二、准备文件

只有两样：

- `Winlator-11.2-SAIPen.apk` —— 补丁版 Winlator（内置压感桥 + 中文字体 + 笔点击支持），从 GitHub Release 下载
- PaintTool SAI Ver.2 64bit ZIP —— 自己购买下载，放进平板的 Download 文件夹

平板的 Download 文件夹在容器里就是 `D:\`，后面在容器里就能看到。

> 旧版 APK 才需要额外准备 `wintab32.dll`、两个 `.reg` 和字体文件，见文末附录。

## 第 1 步：装补丁版 Winlator

1. 平板上允许「安装未知来源应用」。
2. 点开 `Winlator-11.2-SAIPen.apk` 安装。
3. 首次启动如果弹出 16 KB 对齐 / 调试签名 的警告，点「确定 / 继续」，不影响使用。

这个 APK 基于原版 Winlator 11.2 逐字节复制，只替换了 `classes.dex`、新增 `classes2.dex`，并替换了新容器的模板。它做四件事：

① 把笔压通过 UDP 发给容器里的 `wintab32.dll`；
② 把「笔尖接触」翻译成鼠标左键，让笔能点菜单；
③ 新建容器时预置 `wintab32.dll`、压力上限、注册表覆盖和 Noto 中文字体；
④ 万一 Wine 在容器首次启动时把 `wintab32.dll` 换回它自带的版本，会在容器界面起来时自动补回。

## 第 2 步：建容器

### 2.1 新建容器

1. 打开 Winlator → 右上角 ＋ → 新建容器。
2. 名字随便填（例如 SAI）。
3. 屏幕分辨率（Screen Size）选 custom，然后在右边填设备本身的分辨率。不知道的话：截一张图，在相册里看图片详情里的尺寸。
4. DPI 适当调大。参考值：11 英寸 2560×1600 的平板，填 168 左右比较舒服。
5. 其他保持默认，保存。

### 2.2 一个必须改的设置

左上角三横 → ⋮ → Settings（设置）→ 取消勾选 **Capture pointer on external mouse**（捕获外部鼠标指针）→ 点右下角 ✓ 确认。

勾着的话容器会抢走系统光标，数位板事件根本进不来，笔就完全没反应。

### 2.3 第一次进容器

点容器右侧 ▶ 启动。首次会自动初始化 Wine，可能要 1～5 分钟，屏幕可能黑一会儿，看到 Windows 桌面或文件管理器窗口就成功了。

这时容器里已经有了：`C:\windows\system32\wintab32.dll`（我们的，185,607 字节）、`C:\wt_pmax.txt`、`C:\windows\Fonts\NotoSansCJK-Regular.ttc`，以及注册表里的 `wintab32=native,builtin` 和字体替换表。中英文都应该是正常的，不再是方块。

如果中文还是方块，见 Q1。

退出容器请用返回键或右上角 ✕，不要直接划掉 App 或强杀。Wine 要把配置写回磁盘。

## 第 3 步：装入 SAI

本项目不提供 SAI 本体、安装包或授权。请自己合法取得 PaintTool SAI Ver.2（SYSTEMAX 官方发布的是 `sai2-...-64bit.zip`，解压即用，没有安装器）。

1. 把 SAI Ver.2 的 64 位 ZIP 传进平板，解压后把整个目录放进容器，例如 `C:\SAI Ver.2`。
2. 必须保留 `init/` 子目录和 `sai2.ini`——只放 `sai2.exe` 会报"构成 SAI 的文件不存在"。
3. 在容器里双击 `sai2.exe`。第一次启动 Wine 要等 10～20 秒才出窗口。

如果你用的是别人做的汉化版，按那份汉化补丁自己的说明安装；本项目不提供、也不验证任何第三方整合包。

| 现象 | 处理 |
|---|---|
| 报"构成 SAI 的文件不存在" | 目录不完整，缺 `init/` 或 `sai2.ini` |
| 菜单、对话框全是方块 | 中文字体没生效，见 Q1 |
| 能画但"保存"不可用 | 还没有授权证书，见下 |

授权证书（要保存文件才需要）：在 SAI 里 Others → System ID 取本机 System ID，到 SYSTEMAX 授权页用许可号 + 证书密码 + System ID 下载对应的 `.slc`，放到 `sai2.exe` 同目录（新版也读 `settings/` 子目录）。以 SYSTEMAX 官方说明为准；本项目不介入授权，也不提供任何绕过方式。

能打开画布就说明程序本身没问题，压感桥也已经在那了——直接去验收。

## 第 4 步：验收

### 4.1 看 DLL 有没有被加载

用文件管理器打开 `C:\wt_status.txt`，内容应该是：

```
open=1
recv=...   ← 你画几笔它会变大
posted=...
pmax=4095
```

- `open=1` → SAI 已经连上压感桥 ✅
- `open=0` 或文件不存在 → 见 Q2

### 4.2 画一笔

在画布上由轻到重画一条线：

- ✅ 线条粗细随力度变化 = 压感成功
- ✅ 笔能点菜单、工具栏、列表 = 笔点击成功

### 4.3 用笔点 UI

这类笔的硬件特性是：只发悬停和"接触"，从不发鼠标按键。补丁版 APK 会把"笔尖接触"翻译成鼠标左键，所以：

- 笔悬停 → 光标跟着走
- 笔落下 → 等于按下左键（可以拖选区、点菜单）
- 抬起 → 松开

双击：两次快速落下-抬起即可（间隔 < 300 ms）。

## 日常怎么用

1. 打开 Winlator → 左上角三横 → Shortcuts → 点 sai 底下的 ▶。
2. 一条设置改了就废，别碰：Winlator 里不要勾 Capture pointer on external mouse。
3. 建议把常用快捷键设到 SAI 工具栏上——笔点比敲键盘方便。
4. 存盘路径建议放 `D:\`（= 平板 Download），方便用手机文件管理器拿出来。

## 常见问题

先做三件事：① 容器正常退出再重进；② 打开 `C:\wt_status.txt` 看 `open=`；③ 确认 `C:\windows\system32\wintab32.dll` 大小是 **185,607 字节**。八成问题能在这三步里定位。

### Q1：中文全是方块 / 方框

0.3.0 起字体已经内置，正常不该出现。真出现了按顺序查：

1. `C:\windows\Fonts\NotoSansCJK-Regular.ttc` 是否存在（约 19 MB）；
2. 注册表里有没有 `HKCU\Software\Wine\Fonts\Replacements`（值应为 `Noto Sans CJK SC`）；
3. 重启容器（字体只在启动时扫描）。

两条都不对，说明容器是旧版本模板建的 → 见附录。

### Q2：能画但线条粗细不变（没压感）

按顺序查（90% 是前两条）：

1. `C:\windows\system32\wintab32.dll` 是不是 185,607 字节（是 69,632 就是 Wine 自带的那份）；
2. 注册表 `HKCU\Software\Wine\DllOverrides` 下有没有 `"wintab32"="native,builtin"`；
3. `C:\wt_status.txt` 里 `open` 是不是 1；
4. 以上都对还没压感，去 SAI 的 Other → Options → Pen Tablet 看一眼，API 选项是不是被改成了别的（默认就是 WinTab，正常情况下不用动这里）。

### Q2.5：线条有粗细，但重压不再变粗 / 一用力就到顶

八成是 `C:\wt_pmax.txt` 没放（DLL 退回默认 1023，量程只有 1/4）。把内容为 `4095` 的 `wt_pmax.txt` 放进 `C:\`，重启容器，再画一笔对比。判断方法：打开 `C:\wt_status.txt`，`pmax=4095` 才对，`pmax=1023` 就是这个原因。

### Q3：笔能画、但点不了菜单

装错 APK 了——必须是本文这套补丁版。官方原版 Winlator 没有这个合成逻辑。

### Q3.5：换了个容器后，SAI 弹「WinTab 驱动返回了错误」/ 完全没压感

用 0.3.0 及以后 APK 新建的容器不会再有这个问题。如果是旧版 APK 建的容器、或容器是在升级 APK 之前建的，见附录。

### Q4：光标消失 / 笔完全没反应

Winlator 容器设置里 Capture pointer on external mouse 又勾上了，取消即可。

### Q5：每次打开画第一笔，弹 FileError(161) / A file I/O error has occurred

已知问题，非致命：SAI 把"撤销历史"写盘时，Wine 返回了 `ERROR_BAD_PATHNAME`。

- 点掉对话框即可，作画、撤销、保存都正常；
- 已排除权限、磁盘、内存、SELinux；
- 要彻底修只能改 Wine，性价比低。

### Q6：装完 SAI 打不开 / 一进容器就闪退

1. 确认装的是补丁版 APK（早期版本有过 dex 校验错误，已修）；
2. 容器分辨率换成 1280x720 再试；
3. 换一个全新容器重装（最省时间的排查方式）。

### Q7：中文界面怎么切回英文 / 其它语言

打开 `C:\SAI Ver.2\sai2.ini`（用记事本 / 文件管理器打开），改第一段：

```
[Locale]
lang = zh    ; 中文
lang = en    ; 英文
```

改完重启 SAI。

### Q8：笔的落笔位置和光标对不上

别在 Winlator 里开"缩放 / 拉伸"。容器分辨率和板子比例不同会有轻微纵向拉伸；实在在意就调板子的映射区域（高级操作），或者把容器分辨率设成和屏幕一致。

### Q9：抬笔后还在画 / 左键像"粘住"了

0.1.2 起已修：抬笔若只发悬停（压力归零）也算抬起，并且按下后 750 ms 没有新事件会自动松手。如果你还在旧包上，升级 APK 即可（同签名直接覆盖，容器数据保留）。

## 附录：旧版 APK / 老容器怎么补

桥的四样东西都装在容器内部，不跟 App 走。旧版 APK 建的容器、或升级 APK 之前就存在的容器，需要手动补一次。**用 0.3.0 及以后 APK 新建的容器不需要做这一步。**

需要准备：`wintab32.dll`（185,607 字节）、`wt_pmax.txt`（内容 4095）、`1-压感覆盖.reg`、`2-中文字体.reg` + `NotoSansCJK-Regular.ttc`。

1. **放 DLL**：文件管理器 `D:\` → 选中 `wintab32.dll` → 复制 → `C:\windows\system32\` → 粘贴 → 提示替换选 Yes。
2. **放量程文件**：`wt_pmax.txt` 复制到 `C:\`。
3. **导入两个 reg**：左下角 Start → System Tools → Registry Editor → 菜单 Registry → Import Registry File… → Look in 切到 My Computer → 进 `(D:)` → 选 `.reg` → 确定。两个都导。导入后**正常退出容器再重进**（注册表只在正常退出时写盘）。
4. **字体**：`NotoSansCJK-Regular.ttc` 复制到 `C:\windows\Fonts\`，再导入 `2-中文字体.reg`。

检查：`C:\windows\system32\wintab32.dll` 应为 185,607 字节，`C:\wt_status.txt` 里 `open=1`。

> 旧版 APK 新建容器后，Wine 会把 `system32\wintab32.dll` 覆盖回自带的那份（69,632 字节），所以每次新建容器都要重新放一次。新版 APK 会自动补回。

GitHub 仓库：https://github.com/happydemao/sai2-android-pen
