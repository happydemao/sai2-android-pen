# Winlator 上跑 PaintTool SAI Ver.2：分步操作

> 目标设备：8 寸以上安卓平板（推荐 8–12 寸，配触控笔/电磁笔）；手机屏幕可用性很低。
> 安卓版本：8.0+（Winlator 11.x 要求）；RAM ≥ 8 GB 更稳（SAI 大画布吃内存）。
> Winlator 版本：**11.2**（`brunodev85/winlator` Releases）。分支版（Cmod / Bionic / Frost / Ludashi）也可，菜单名略有差异。

---

## 1. 安装 Winlator

1. 从 GitHub Releases 下载 `Winlator_11.2.apk`：<https://github.com/brunodev85/winlator/releases>
2. 允许"安装未知来源"，安装并打开。
3. 首次启动会解压 OBB/镜像，**要等 1–5 分钟**，别中途退出。
4. 首次运行会提示缺少存储权限 → 全部允许（否则找不到你的 exe）。

> 提示：安卓 13+ 建议把 Winlator 的电池优化设为"无限制"，否则锁屏后被系统杀掉。

---

## 2. 把 sai2.exe 传到平板上

任选一种：

- **数据线 / QQ / 微信 / 网盘**：把 `sai2.exe` 放到平板 `Download` 目录。
- **同时带走笔刷和色板**（强烈建议）：把 PC 上的
  `%APPDATA%\SYSTEMAX Software Development\SAIv2\`
  整个文件夹也拷到 `Download\SAIv2\`。

---

## 3. 新建容器（Container）

Winlator 主界面 → 右上 `+` → 新容器 → 按下表填：

| 项 | 建议值 | 为什么 |
|---|---|---|
| Container name | `SAI2` | — |
| Screen size | `1600x900`（8–10 寸）/ `1920x1200`（11 寸以上） | SAI 面板多，太小的分辨率面板会重叠 |
| **Screen DPI** | **120–144**（先试 132） | SAI 声明了 DPI-aware（manifest `dpiAware=true/PM`），96 会让按钮窄到手指点不中；太高面板会溢出屏幕 |
| Graphics driver | `Turnip`（骁龙）/ `VirGL` 或 `LLVMpipe`（其他 SoC） | SAI **不使用任何 GPU API**，只走 GDI，这里选什么都行，选最稳的即可 |
| DX wrapper | `WineD3D`（或 `None`） | 无 DirectX，避免 DXVK 多余开销 |
| **Box64 preset** | **`Compatibility`（不稳就换 `Stability`）** | SAI 用 SSE 做像素运算，激进优化（`Performance`/`Intermediate`）容易崩或画错 |
| Box64 version | 默认即可 | — |
| Audio driver | `ALSA`（默认） | 有提示音，无所谓 |
| Windows version | `Windows 10` | exe manifest 声明支持 Win7–Win10 |

容器创建后 → `Container Settings` → **Environment Variables**，加入第 5 节的变量。

---

## 4. 映射目录并启动

1. `Container Settings` → **Drives** → `D:` 指向 `/storage/emulated/0/Download`（或你的下载目录）。
2. 启动容器 → 进入 Windows 桌面 → 双击 `D:` → 找到 `sai2.exe` → 双击。
3. **首次启动会慢**（Box64 要生成缓存，几十秒到几分钟），别以为卡死了。
4. 想省事：在桌面给 `sai2.exe` 建快捷方式，以后从 Winlator 主界面的快捷方式直接进（还能单独配参数）。
5. **恢复笔刷/色板**：把 `SAIv2` 文件夹放到容器的
   `C:\users\<你>\AppData\Roaming\SYSTEMAX Software Development\SAIv2`
   （Wine 的 AppData 在容器盘符里，可用 Wine 自带 `explorer` 定位）。

---

## 5. 环境变量（`Environment Variables` 页）

见 [`box64.env.example`](../box64.env.example)。核心几条：

```
BOX64_DYNAREC_BIGBLOCK=0      # SAI 代码量大、跳转多，大 basic block 容易出错
BOX64_DYNAREC_SAFEFLAGS=2     # 标志位严格模式，防"画出来的像素错位/颜色错"
BOX64_DYNAREC_STRONGMEM=1     # 强内存序，防偶发崩溃/黑屏
BOX64_DYNAREC_FASTNAN=0
BOX64_DYNAREC_FASTROUND=0
MESA_EXTENSION_MAX_YEAR=2003  # 老式 GDI 程序偶尔需要更老的 GL 扩展集（保守项）
```

- 先按上面跑；如果**能起但很卡**，再逐条把 `BIGBLOCK` 调回 `2/3` 试。
- 如果**起不来/闪退**，先把 preset 换 `Stability`，再考虑 `BOX64_DYNAREC_X87DOUBLE=1`。

---

## 6. 输入与操作（重点）

| 需求 | 做法 |
|---|---|
| 单指画画 | 输入模式选 **`Touchscreen`（触屏直通）**，手指/笔 = 鼠标左键 |
| 需要右键/滚轮（菜单、缩放） | 切换成 `Touchpad` 模式，或用屏幕上的虚拟按键（Winlator 输入控制里加"右键/滚轮"按钮） |
| 双指缩放画布 | SAI 里是 `Ctrl+滚轮` / 空格+拖动。**建议在虚拟按键里映射 `Ctrl` + `滚轮`**，比双指靠谱 |
| 笔压 | ❌ **不支持**。SAI 走 Windows Ink（`WM_POINTER`）拿压力，Winlator 只转鼠标事件，压力恒为固定值 |

**压感缺失的临时对策**（按有效性排序）：

1. 用**固定笔刷 + 大小抖动**：SAI 的笔压参数虽失效，但"笔刷浓度/大小"手动调节 + 多次叠色仍能出效果。
2. 用 SAI 的**「笔压 → 不透明度」关掉**，改用"笔刷浓度 + 图层不透明度"分层画。
3. **真的想要压感**：SAI 还有一条 **WinTab（`wintab32.dll`）** 通道，容器里放一个自制 DLL 就能接管笔压，
   安卓侧由 Winlator 补丁把 `getPressure()` 喂进去。全套方案（含已验证的报文格式与现成代码）见
   **[ANDROID-PRESSURE-PORT.md](./02-bridge-design.md)**。
4. 最省事：走 **README 路线 D（Sunshine + Moonlight 串流）**，压感 100% 保留，且没有性能损失。

---

## 7. 性能调优（卡的时候按顺序试）

1. 画布别开太大：**2000×2000 内**体验最好，4000×4000 以上拖动会明显掉帧。
2. 减少图层数（每个图层都是内存位图 + 合成开销）。
3. 关闭 SAI 的「预览/缩略图」类面板。
4. 容器 `Screen size` 降一档（分辨率对 GDI 搬运成本影响是平方级的）。
5. Box64 preset 从 `Compatibility` → `Stability`（更慢但更少崩）。
6. 关掉其他后台 App，平板开性能模式。

---

## 8. 常见故障对照表

| 现象 | 原因 | 处理 |
|---|---|---|
| 双击没反应，任务管理器里进程在 | Box64 首次编译缓存慢 | 等 1–3 分钟；第二次会快 |
| 闪退，日志有 `SIGSEGV` | 无 `.reloc` 段 + 固定基址加载冲突 | 换 Box64 preset `Stability`；或在 Wine 配置里关闭 "Enable ASLR" 类选项 |
| 启动报缺 DLL | 正常不会（只依赖 system32） | 若报 `msvcrt`，说明你下到了别的版本；本 exe 静态链接 CRT |
| 界面字体是方块/乱码 | 容器缺中日文字体 | 把 PC 的 `C:\Windows\Fonts\msgothic.ttc`、`msyh.ttc` 拷进容器 `C:\windows\Fonts\`，重启容器 |
| 画面花屏/错位 | GDI 与显示驱动不匹配 | 换 Graphics driver；或容器设 `Force Fullscreen` |
| 画一笔延迟半秒 | 画布过大 / CPU 弱 | 见第 7 节 |
| 笔迹断线 | 触摸事件被系统吞 / 省电限制 | 关电池优化；输入模式改 `Touchscreen` |
| 保存的 `.sai2` 在 PC 打不开 | 版本差异（Alpha 版向前不兼容） | 用同版本 SAI 打开；或导出 PNG/PSD 传递 |

---

## 9. 已知边界（别抱错期待）

- 这不是"移植版 SAI"，是"**在安卓里跑原版 exe**"。启动慢、发热、耗电都真实存在。
- **没有压感 = 对画画是致命短板**。如果你的核心诉求是"在平板上用 SAI 画线稿"，串流方案（路线 D）会明显更爽。
- 商业授权上：个人自用无问题；**分发/打包他人 exe 或修改后的镜像**请注意版权。
