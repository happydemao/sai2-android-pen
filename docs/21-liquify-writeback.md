# 液化插件：SAI 画布写回路径（2026-11-21 夜）

这一页接着 `20-canvas-capture.md` 往下做，只写**这一轮实测到的**东西。
目标不变：在 `lator.forsai` 的 SAI Ver.2 里做出可用的液化插件。

## 一、先修掉上一轮的错误：.pdata 边界算错了

上一轮 `containing(0x1402552d9)` 返回 `0x2552d9` 本身，原因是把 x64 的
**chained unwind 记录**（同一条 `RUNTIME_FUNCTION` 链上的后续片段）当成了独立函数。

`UNWIND_INFO` 的第一个字节是 `Version:3 | Flags:5`，`UNW_FLAG_CHAININFO`（0x4）
落在 **bit 5**（即 0x20）。按这个位过滤后：

| 地址 | 正确函数范围 |
|---|---|
| `0x14025326C`（SetDIBitsToDevice 调用点） | `0x140252F30 .. 0x1402532E7` |
| `0x140255350`（CreateDIBSection 调用点） | `0x140255290 .. 0x1402553C7` |

工具：`liquify/probe/pe_dis.py`（需要 `capstone`/`pefile`，装到 `.pylibs/`）

```bash
PYTHONPATH=.pylibs python3 liquify/probe/pe_dis.py containing 0x14025326c
PYTHONPATH=.pylibs python3 liquify/probe/pe_dis.py callers    0x140252f30
PYTHONPATH=.pylibs python3 liquify/probe/pe_dis.py chain      liquify/notes/wt_stack.log
```

## 二、呈现路径（栈回溯实测）

在 `SetDIBitsToDevice` hook 里抓调用栈，再用「返回地址前一条指令必须是指向下一层函数
的 call」逐帧验证（`pe_dis.py chain`）：

```
0x1402532F0  呈现叶子（217B，负责 32bpp 行拷贝 + SetDIBitsToDevice）
0x140255950  包一层 RECT 的封装
0x1401FA6C0  把表面贴到窗口（GetWindowRect → 计算矩形 → 调用封装）
0x14018ADE0  再上一层调用者
```

注意：`SetDIBitsToDevice` 实际有 3 个调用点，分别落在 3 个**独立**的小函数里
（`0x1402532F0` / `0x1402533D0` / `0x140252F30` 里各一个），不是同一个函数。

## 三、PAGE_GUARD + VEH 在 Box64 下是可用的（已验证）

上一轮怀疑 Box64 走不到 VEH。这一轮做了两个对照实验：

1. **VEHTEST**：自己 `VirtualAlloc` 一页 → 上 `PAGE_GUARD` → 自己写 →
   VEH 命中，`rip` 在 `wintab32.dll`。**机制本身成立。**
2. **GUARDPOKE**：对 `CreateDIBSection` 返回的 bits 页上 guard 再写 →
   同样命中。**DIB 的 section 视图也能拦住。**

所以上一轮「守 DIB 没有命中」不是机制问题，而是**守错了页**（日志里守的是
`0x9CC1000`，而当时的 DIB 是 `0x8D70000`）。

## 四、抓到了「谁把像素写进画布 DIB」

做法：`C:\wt_guard_whole` 一次性 guard 整个 DIB，然后在画布上画一笔。

```
WATCH hit fault=09D695C0 rip=140234EDE rdi=9d695c0 rsi=137d7800
```

- `rip = sai2.exe+0x234EDE`，位于 **32bpp 行拷贝器** `0x140234EA1` 的 `rep movsd`
- 这次写入是 **16×16 的瓦片**：每次命中 `rdi` 下移一行（+0x3800=14336B=DIB 行距），
  `rsi` 下移 0x40（16px × 4B），连续 16 行
- 也就是说：**SAI 不是一次性整面重绘，而是把 16×16 的块从源缓冲拷进视口 DIB**

`0x140234EA1` 的签名（行拷贝，宽高单位是像素，步长单位也是像素）：

```
rcx = dest, [rbp+0x50] = src, [rbp+0x58] = src stride
edx = dest stride, r8d = width, r9d = height
```

同一族的还有 `0x140234E43`(16bpp) / `0x140234DE6`(8bpp) / `0x140234F5C`(24bpp)，
都在呈现函数里被按 bpp 分派。

## 五、表面对象布局（实测确认）

从栈里拿到源对象指针后 dump：

```
obj+0x00  refcount(1) / 格式码
obj+0x08  w        = 800
obj+0x0c  h        = 800
obj+0x10  stride   = 3200 (字节)
obj+0x18  bits     = 0xDF30000   ← 800×800 BGRA 画布视图（纸上像素 f8f8f8 00）
obj+0x20  第二缓冲 / 另一份位图
```

这跟 `0x1402553D0`（表面工厂）里的写入完全一致：`+0x10` HBITMAP、`+0x18` 像素、
`+0x28` BITMAPINFO。

**重要：找到的是 800×800 的「视图表面」，不是 1600×1600 的文档表面。**
在 1600×1600 文档 + 50% 缩放下，视图就是 800×800。

## 六、写回持久性：视口 DIB 不持久（再次实测确认）

本轮用导出 PNG 做了闭环验证：

1. 新建 1600×1600 文档，画三笔；
2. `C:\wt_write` 往视口 DIB 中心写 320×320 品红（BGRA `FF00FFFF`）；
3. 屏幕上确实出现品红；
4. 文件 → 另存为 → 选 `png` → 导出 `save1.png.png`；
5. 拉回来逐像素统计：**品红像素 = 0**，非白像素 = 2309（正好是三笔）。

结论：**视口 DIB 只是呈现缓冲，不是文档数据。** 这条路径不能用来做液化。

## 七、探针新增的触发文件（都在 `drive_c` 下）

| 触发文件 | 作用 |
|---|---|
| `wt_mouse.txt`（`x0 y0 x1 y1 steps delay`） | 进程内 `mouse_event` 注入一笔，解决 `adb input` 拖拽进不了画布子窗口的问题 |
| `wt_blits` | 打印最近 96 次 `SetDIBitsToDevice`（bits/矩形/源坐标），看清脏矩形重绘 |
| `wt_stack` | 在下一次画布呈现时抓调用栈（只 memcpy，不做文件 IO，避免卡顿） |
| `wt_guard_canvas`（`off npages`） | 在 DIB 内指定偏移挂 N 页 `PAGE_GUARD` |
| `wt_guard_whole` | guard 整个 DIB（命中后打印 rip/寄存器/栈窗口） |
| `wt_guard_poke`（`off`） | DIB 页 guard 对照实验 |
| `wt_surfaces` | 全进程扫描 `{w,h,stride,bits}` 表面对象，输出 `wt_surfaces.log` |
| `wt_vehtest` | PAGE_GUARD+VEH 对照实验 |

脚本：

- `build_probe.sh`：LLVM-mingw 编译 + strip + 补零到 **94720 字节**（否则启动时会被
  `PenBridge.ensureBridgeFiles()` 换回发行版）
- `deploy.sh`：先拷 DLL 再 `force-stop`（顺序反了会加载到旧 build），最后调用
  `launch_sai.sh`
- `launch_sai.sh`：按当前屏幕方向点快捷方式的播放键，并用 `wt_status.txt` 的
  mtime 确认 SAI 真的起来了（上一轮盲点坐标打开过明日方舟）
- `open_doc.sh`：关掉崩溃恢复对话框并新建 1600×1600 画布

## 八、下一步（按价值排序）

1. **修 `wt_surfaces` 的地址 bug**（已改：`base+off+i`，并放宽白色判定到
   `B,G,R ≥ 0xF0`），重新扫一遍，找 **1600×1600 / stride 6400** 的表面。
   - 找到 → dump 像素，确认里面有三笔 → 直接在这块上做液化；
   - 找不到 → 说明图层是瓦片存储（`20-canvas-capture.md` 第七节的
     stride-1024 小块），需要逆瓦片编解码。
2. 若找到文档表面：**写一个标记像素 → 导出 PNG → 看是否带标记**。
   这一步才是「持久性」的真正判据。
3. 持久性成立后，把 `liquify/engine/lq.c` 接上去：
   - 在 `SetDIBitsToDevice`/呈现 hook 前后做视图层的实时预览；
   - 落笔结束时把结果写回文档表面；
   - 加速度只作用在脏 tile。
4. 五种模式（向前变形/膨胀/收缩/旋转/平滑）已在 `lq.c` 里实现，引擎单测在
   `docs/20-canvas-capture.md` 第六节有数据（2560×1600 大画布 40 步 18.2ms）。
5. 之后再谈多图层/文件夹/选区/蒙版：这些都需要先拿到「图层列表 + 每层表面」，
   也就是第 1 步的自然延伸。

## 九、写回路径成立：剪贴板 → 粘贴成图层（实测，00:05）

绕开私有瓦片格式的写回路径**已经跑通**：

1. 在 `wintab32.dll` 里 `OpenClipboard` + `SetClipboardData(CF_DIB)` 放一张
   256×256 的测试图（绿底方块 + 品红边）；
2. `adb` 发 **Ctrl+V** 给 SAI；
3. SAI **新建了「图层2」**，图像放在文档左上角，1:1 文档像素（不是 50% 视图像素）；
4. 文件 → 另存为 → PNG，导出 `clip.png.png`；
5. 逐像素统计导出的 1600×1600 PNG：`nonwhite = 65536 = 256×256`，
   其中绿 30625、品红 34911 —— **完全一致，写回真正进了文档**。

结论：

- 视口 DIB 只能做**实时预览**（不持久，第六节已证）；
- **提交**走 `CF_DIB → 粘贴`，得到的是 SAI 自己的图层，保存/导出/撤销都由 SAI 负责；
- 粘贴不改变原图层，天然是非破坏性的；需要「替换」时可以再合并/隐藏原层。

这一刻起，液化插件的两条腿都齐了：

```
预览：hook SetDIBitsToDevice / 直接改视口 DIB（lq 引擎按 64×64 tile 重建）
提交：CF_DIB → Ctrl+V → 新图层（SAI 原生保存）
```

## 十、还差的一块：文档分辨率像素

预览和提交都需要**文档分辨率**的像素，而视口 DIB 在 50% 缩放下只有 800×800。
处理办法（下一步实现）：

1. 液化开始时把缩放切到 **100%**（SAI 的缩放框可输入百分比，或用快捷键），
   此时视口 DIB 里的画布区域与文档 1:1；
2. 按视口大小分块抓取（3584×2560 的 DIB 一次就能覆盖 1600×1600 文档）；
3. 处理完把缩放恢复；
4. 提交时粘贴同样是 1:1，落点就是文档原点（本次 256×256 实验已证实）。

## 十一、五种模式的分工（下一步）

- **实时预览**：`lq.c` 的五种模式在 DLL 内按脏 tile 重建，只作用在视口 DIB；
- **最终结果**：Android 端（ARM 原生）对文档分辨率图做同一套位移场重建，速度比
  Box64 下的 x86 代码快一个量级；结果通过本地 socket / 共享内存交回 DLL；
- **提交**：DLL 把结果包成 `CF_DIB`，`Ctrl+V`。

`lq.c` 目前是纯 C99、无 libc 分配，同一份代码可以：
- 编进 DLL 做预览；
- 用 NDK 编成 `libliquify.so` 给 app 的 JNI 用（Android 端处理）；
- 在 Mac 上跑单测。

## 十二、这一轮又验证的两件事（2026-11-22 00:20）

### 1. 在视口 DIB 里「找画纸」不可靠

试过三种办法在 3584×2560 的视口 DIB 里定位 800×800 画纸：

- 按 `g_cv_bits` + 子窗口客户区 + 最后一次 blit 的 `src-dest` 偏移换算：
  x 对得上（1392），y 对不上（预测 479，实测 880），说明 blit 偏移不能当全局平移用；
- 全 DIB 扫最长白色 run：找到一块 800×800 纯白块，但**没有笔迹**——
  视口 DIB 里有多块等大的白色缓冲（未使用的后备存储也是白的）；
- 按「白色区域里必须含墨」筛选：分块在笔迹处被切断，仍然选不出完整画纸。

结论：**不要再从视口 DIB 里反推画纸位置**。改为用下一节的剪贴板读路径。

### 2. SAI 的 Ctrl+A / Ctrl+C 会往剪贴板放 CF_DIB

在 SAI 里发 `Ctrl+A`（全选）+ `Ctrl+C`（复制），进程内读剪贴板得到：

```
CLIPREAD: 802x702 bpp=24 comp=0 imgsz=1690416 hdr=40
```

- 是标准 `CF_DIB`（24bpp，无 alpha），SAI 主动放进剪贴板的；
- 尺寸 802×702 与 50% 缩放下的视口区域接近，**待验证**：
  在 100% 缩放下复制是否得到 1600×1600（即文档分辨率）。
  如果成立，读路径就完全不用碰内存扫描。

对应的探针命令：`wt_clipread`（读 CF_DIB，32bpp 时落盘 `wt_clipin.bin`）。
下一步先把 24bpp 也落盘，再测 100% 下的尺寸。

### 3. 这一轮新增的探针命令

| 触发文件 | 作用 |
|---|---|
| `wt_doccap` | 从所有已记录的大 DIB 里找「含墨的白色画纸」并裁剪导出 `wt_doc.bin/.bmp/.txt` |
| `wt_paste`（`wt_paste.txt` + `wt_paste.bin`） | 把 BGRA 图放上剪贴板，等 Ctrl+V |
| `wt_clipread` | 读剪贴板 CF_DIB 到 `wt_clipin.bin` |
| `wt_clip` | 放一张 256×256 测试图（已用于验证粘贴路径） |

`probe_CreateDIBSection` 现在会记录每个 ≥256×256 的 32bpp DIB（最多 8 个），
供 `wt_doccap` 遍历。

### 4. 踩坑记录

- 改缩放时误点到了窗口的「还原」按钮，SAI 变成 690×430 的小窗；
  `Alt+Space` / 点最大化按钮在 Winlator 的 touchpad 模式下不生效，
  最后靠 `deploy.sh` 重启容器恢复。
- 命令行里手算十六进制地址连续出错两次，往错误的地址 poke 过 80KB
  （幸好落在空闲内存，SAI 未受影响）。**以后一律用 python 算地址再传。**

## 十三、下一轮要做的事（明确的执行顺序）

1. `wt_clipread` 支持 24bpp 落盘；把复制得到的图转成 PNG 看内容，
   确认 `802x702` 到底是什么（画纸？视口？带边框？）。
2. 在 100% 缩放下重复：如果得到 1600×1600，读路径确定；
   若仍是视口尺寸，就改走「临时切 100% 再复制」或导出 PNG。
3. 写 `wt_lq` 原型：
   - 从 `wt_clipin.bin` 载入文档图；
   - 用 `lq.c` 按鼠标轨迹做五种模式；
   - 结果写 `wt_paste.bin` → 剪贴板 → Ctrl+V → 导出验证。
   这一步先不做实时预览，先跑通「处理 → 写回 → 导出」的闭环。
4. 实时预览再单独解决：优先考虑在画纸上方叠一个 `WS_EX_LAYERED` 细窗，
   避免继续和 SAI 的 DIB 布局纠缠。

## 十四、缩放与复制路径的补充观察（00:25）

- SAI 的缩放是个带 `-` / `+` 的数值框，步进是 **1.5×**（8.33 → 12.5 → … → 66.7 → 100 → 150）；
  直接点 `+` 或 `-` 比输入数字可靠（输入 100 不生效）。
- 工具栏位置（2560×1600 最大化时）：`-` 在 (1102,110)，`+` 在 (1133,110)。
- 在 150% 缩放下 `Ctrl+A` / `Ctrl+C` 后读剪贴板得到 **no CF_DIB**——
  空文档（没有像素内容）不会复制出剪贴板图像。**复制前画布上必须有内容**，
  这也解释了第一次 802×702 的来源：那是「50% 缩放下带笔迹的可见画布」。
- 因此新计划：复制前先在画布上画一笔（或让用户至少有一笔），再判断复制的尺寸；
  若 100% 下仍是 800×800 级别，就说明 SAI 按**视口分辨率**复制，
  需要改成「临时切 100% + 横向/纵向拼接」或者在 100% 下逐屏复制拼接。

# 液化闭环跑通（2026-11-22 00:26）

## 结果

```
SAI Ctrl+A / Ctrl+C        -> CLIPREAD: 1600x1600 bpp=24  (文档分辨率，已验证)
wt_lq (forward)            -> LQ: bbox=504,599..1101,1001  608ms
                              PASTE: 1600x1600 CF_DIB placed
SAI Ctrl+V                 -> 图层3（新图层，1:1 落在文档原点）
另存为 PNG                 -> warp.png.png (1600x1600)
```

导出 PNG 里逐行量红线位置（原图是 y=x 的对角线）：

| y | 600 | 650 | 700 | 750 | 800 | 850 | 950 | 1000 |
|---|---|---|---|---|---|---|---|---|
| x | 600 | 654 | 767 | 854 | 893 | 914 | 954 | 1000 |

笔刷参数 `forward 700 800 900 800 radius=200 strength=0.8`，红色在 y=800 附近
被推了约 +100px，两端回到对角线——**和引擎的变形场完全一致，而且确实进了保存文件。**

## 这条闭环的完整链路

```
Ctrl+A / Ctrl+C  ──►  CF_DIB (1600x1600, 24bpp)  ──►  wt_clipread（归一成 32bpp BGRA）
                                                         │
                                                   lq.c 五种模式
                                                         │
wt_paste（CF_DIB）◄──  wt_paste.bin  ◄───────────────────┘
        │
   Ctrl+V  ──►  SAI 新图层  ──►  另存为 / 导出 PNG（SAI 原生，像素确认）
```

一键脚本：`liquify/probe/lq_run.sh "twirl_cw 800 800 800 800 250 0.8 40"`

## 已知限制

1. **读的是「拼合后的可见结果」**：Ctrl+A/Ctrl+C 拿到的是画布合成结果，
   不是单独某个图层。所以液化作用在拼合图上，提交后是新图层（原图层保留）。
   要支持「只液化当前图层」，需要先隐藏其它图层再复制（UI 自动化），
   或者后续逆出图层表面（`wt_surfaces` 已经能看到表面对象，但还没找到图层表）。
2. **没有实时预览**：现在是「一次算完 → 粘贴」，交互上是一步到位。
   下一步要做笔刷式交互（按下-拖动-松开），预览建议用叠加窗。
3. **Box64 下 608ms/40 步**（1600×1600）：能用，但 Android 端 ARM 原生会快一个量级，
   这正是「Android 端处理」那一步要拿到的收益。
4. 粘贴出来的是新图层，SAI 里可以再手动合并/隐藏原层；插件后续可以自动做。

## 下一步

1. 把 `lq.c` 用 NDK 编成 `libliquify.so`，JNI 暴露 `lq_stroke_begin/apply/flush`，
   在 app 侧完成像素处理（同一份 C 源码，x86 和 arm64 共用）。
2. app 侧传输：DLL 把 `wt_clipin.bin` 通过本地 socket / 共享文件交给 app，
   app 处理完回传 BGRA，DLL 负责放剪贴板 + Ctrl+V。
3. 交互：工具栏（五种模式、半径、强度）+ 画布拖动采样 + 叠加窗预览。
4. 多图层：先做「只显示目标层 → 复制 → 液化 → 粘贴」的图层隔离流程，
   文件夹/选区/蒙版在此基础上扩展。

## 五种模式的引擎单测（2026-11-22 00:32）

`liquify/engine/lq_test.c` 是纯 C 单测，不依赖 Wine/设备：

```bash
cc -O2 -std=c99 -D_DARWIN_C_SOURCE -o /tmp/lq_test \
   liquify/engine/lq.c liquify/engine/lq_test.c -lm
/tmp/lq_test
```

```
forward      PASS   cx 104.5 -> 113.5 (dx 9.0)
bloat        PASS   cx 104.5 -> 107.7
pinch        PASS   cx 104.5 -> 102.7
twirl_cw     PASS   y 127.5 -> 146.4      # 屏幕 y 向下，顺时针 = y 增大
twirl_ccw    PASS   y 127.5 -> 108.8
smooth       PASS   pure red 100 -> 0
idempotent   PASS   flush twice
undo         PASS   restores snapshot
ALL PASS (0 failures)
```

**这轮单测抓到一个真 bug**：`LQ_TWIRL_CW` 的实现方向反了。
屏幕坐标 y 向下，`sample = R(+ang)·(p-C)` 会让内容逆时针转；
现在 CW 用负角，并加了「右→下 = 顺时针」的方向断言。

bloat/pinch 的位移量随步数饱和（内容被推出笔刷半径后质心指标失效），
所以单测只用 3 步验证方向，不验证幅度。

# Android 端处理接通（2026-11-22 00:50）

## 架构变了：像素处理搬进 app

```
SAI Ctrl+A/C  ->  CF_DIB 1600x1600
wt_clipread   ->  wt_clipin.bin（32bpp BGRA）
wt_lqnet      ->  127.0.0.1:47801  ->  LiquifyServer（app，Java）-> LiquifyWarp
               <-  变形后的 BGRA
wt_paste      ->  CF_DIB  ->  Ctrl+V  ->  SAI 新图层
```

DLL 不再自己算像素，只做「复制/粘贴/发 Ctrl+V」；五种模式跑在 ARM 原生的
Java 里（`android/LiquifyWarp.java`，和 `liquify/engine/lq.c` 同一套模型）。

## APK 打补丁流水线（macOS，无需 Gradle/pwsh）

`android/build-apk.sh`：

1. `apktool d -r` 解包**干净**的发行 APK（`Winlator-for-sai-0.3.30-...apk`）
2. 删掉解码树里残留的 `Liquify*.smali`
3. 在 `TouchpadView.updateXform` 的 `PenBridge.configure` 后注入
   `LiquifyServer.start()`
4. `apktool b` 重打包
5. `javac` + `d8` 编译 `LiquifyWarp/LiquifyServer` → 新 dex
6. 追加为 `classesN+1.dex`（apktool 可能已经生成 classes3，重名会被
   apksigner 拒绝，脚本自动取下一个序号）
7. `zipalign` + `apksigner`（`keys/debug.keystore`，SHA-256 `02:01:22:...`，
   与已安装签名的同一个）→ `adb install -r`，容器数据保留

踩过的坑：

- 一开始拿「已经打过补丁的 APK」当 base，解包出来的 smali 里带着旧的
  LiquifyServer，重打包后旧的 classes3.dex 会**抢先**被类加载器选中，
  新代码永远不生效（表现为协议对不上、时间字段恒为 0）。
- 协议是大端（Java `DataInputStream`），第一版 DLL 发的是主机小端，
  服务端直接关闭连接。
- apktool 在 `$HOME/Library/apktool` 建 framework 目录，沙箱下会失败；
  脚本把 `HOME` 指到 `/private/tmp/apkhome`。

## 实测

| 项目 | 结果 |
|---|---|
| 协议端到端 | `LQNET: mode=forward 1600x1600 round trip 842ms` |
| 导出 PNG 验证 | 红对角线 y=700→767、750→854、800→893、1000→1000，与 x86 引擎**完全一致** |
| 桌面 JVM 基准 | forward 12–31ms；twirl/bloat/pinch/smooth 19–76ms |
| 平板（warm） | forward java warp 54ms、round trip 82ms；twirl 115ms；pinch 74ms |
| 平板（首次） | 仍有一次性 ~3.2s（ART 首次大缓冲/方法编译），**待下轮查** |

优化：把 `sqrt/sin/cos` 全部搬出内层循环，用「d² → 权重/旋转/径向缩放」
的 1024 项查找表，twirl 从 2.4s 降到 115ms（warm）。

## 目前仍未完成

1. **首次调用 3.2s**：prewarm 已经加在服务线程启动时（3 次 64×64），
   但第一次 1600×1600 仍然慢，需要把「读图/分配/变形」分段计时。
2. **交互与实时预览**：现在是「一次性算完 → 粘贴」，没有笔刷拖动和叠加窗预览。
3. **多图层/文件夹/选区/蒙版**：读的仍是拼合结果，提交成新图层。

## 跨实现逐像素等价性（2026-11-22 01:15）

`liquify/engine/lq_dump.c`（C 参考）与 `android/LiquifyCompare.java`（Java 移植）
用**同一张输入图 + 同一组笔迹参数**各跑一遍，Python 逐像素对比 256×256 输出：

| 模式 | 差异像素 | max\|Δ\| |
|---|---|---|
| forward | **0** | 0 |
| bloat | **0** | 0 |
| pinch | **0** | 0 |
| twirl_cw | **0** | 0 |
| twirl_ccw | **0** | 0 |
| smooth | **0** | 0 |

做到零差异踩过的四个点（都已修）：

1. **smooth 算法不同**：C 是「每步按 falloff 半径做盒式模糊，再按权重和原像素混合」，
   Java 一开始只在整个包围盒上做了一次固定半径模糊 → 重写成逐步混合。
2. **位移场量化**：C 存 `int16`（1/16 px），Java 用 float 累加 → 按 1/16 量化。
3. **累加精度**：C 用 `double` 累加 + `lrint`（就近取偶），Java 用 float 会差 1/16 px；
   改成 double + `Math.rint` 后 forward 的 73 个边缘像素差异归零。
4. **步进增量**：Java 必须像 C 一样用「本次位置 - (位置 - 步长)」求 mdx/mdy。

复现：

```bash
cc -O2 -std=c99 -o /tmp/lq_dump liquify/engine/lq.c liquify/engine/lq_dump.c -lm
/tmp/lq_dump /private/tmp/lqref
javac -d /tmp/ltest android/LiquifyWarp.java android/LiquifyCompare.java
java -cp /tmp/ltest com.winlator.sai.LiquifyCompare /private/tmp/lqjava
# 然后逐字节比较两目录下同名 .bin
```

结论：Android 端不是「差不多的重写」，而是**与 DLL/参考引擎逐字节一致的移植**。

## 进程内一键液化（2026-11-22 01:35）

不再用 adb 发按键：`C:\wt_auto` 触发后，DLL 自己完成整条链：

```
AUTO: foreground=sflRootWindow|PaintTool SAI Ver.2 ...
CLIPREAD: 1600x1600 ...
LQNET: forward 1600x1600 round trip 2938ms (java warp 2917ms)   # 冷
PASTE: 1600x1600 CF_DIB placed
AUTO: done (pasted as a new layer)
```

第二次触发：`round trip 46ms (java warp 33ms)`。

实现：`SendInput` 发 Ctrl+A / Ctrl+C → 轮询剪贴板拿到 CF_DIB →
`lq_net_ex("C:\\wt_lq.txt")`（app 端 Java 引擎）→ 失败则回退到本地 lq.c →
`doc_paste()` → `SendInput` Ctrl+V。

**冷启动 2.9s 的原因**：`LiquifyWarp.warp` 首次调用还在解释执行（ART 的 JIT
要够多次调用才编译），预热只跑了 3 次 64×64，没到阈值。第二次起 33–45ms。
下一步把预热改成足够多次的小图调用。

验证：截图里 图层3 是粘贴进来的结果，红线中段被推出 S 形弯，和参数一致。

## 性能真相：debuggable 应用不会 AOT（2026-11-22 01:25）

实测 `cmd package compile -m speed -f lator.forsai` 返回 Success，但
`dumpsys package` 里仍然是：

```
arm64: [status=verify] [reason=cmdline] [primary-abi]
flags=[ DEBUGGABLE ... ]
```

**debuggable 的应用会被系统强制成 verify（只校验、不编译）**，所以我们的
`LiquifyWarp` 一直靠解释器执行：

- 1600×1600 forward / 40 步：热身后仍然 **~900ms**；
- 同一份算法在桌面 JVM 只要 8–12ms（差 ~75 倍，只能解释为没编译）；
- 对比 Box64 跑 x86 版 `lq.c`：~600ms。

所以「Android 端处理」目前只是**功能正确**（逐字节等价），**性能并没有赢过
Box64**。拆小方法、预热 12000 次、AOT 编译都没用，因为 debuggable 直接挡住了 AOT。

### 三条出路（下一轮选）

1. **NDK 原生库**（最彻底）：`lq.c` 已经是纯 C99，直接编 `libliquify.so` + JNI，
   完全绕开 ART。代价是下载 NDK（~1 GB）。
2. **非 debuggable 构建**：去掉 `android:debuggable`，`pm compile -m speed` 就能
   生效；但 `run-as` 失效，探针脚本无法再读写容器，需要另找调试通道。
3. **改用本地 x86 引擎提交**：保持 600ms，牺牲「Android 端处理」这个目标。

下一步倾向 1（NDK），因为它同时解决性能和「Android 端处理」的正确性。

## NDK 原生库：问题解决（2026-11-22 01:32）

把同一份 `lq.c` 编成 `libliquify.so`（arm64），通过 JNI 调用，彻底绕开 ART：

| 实现 | 1600×1600 forward / 40 步 |
|---|---|
| Java（debuggable → 解释执行） | ~900 ms |
| Box64 跑 x86 `lq.c` | ~600 ms |
| **NDK arm64 `lq.c`** | **63–71 ms** ✅ |

整条链（复制 + 10MB×2 传输 + 变形 + 写回剪贴板）round trip **82–89 ms**。

### 加载原生库踩的 4 个坑（全部在 `build-apk.sh` 里修掉）

1. **16 KB 页对齐**：Android 15+ 要求 `.so` 的段按 16 KB 对齐，否则 dlopen 报
   「ELF alignment check failed」。链接加 `-Wl,-z,max-page-size=16384`。
2. **sincosf 不存在**：编译器把 `sinf/cosf` 合并成 `sincosf`，而 app 的
   targetSdk=28 看不到这个符号（Android 按 targetSdk 过滤 libm 符号）。
   加 `-fno-builtin`。
3. **ceilf 找不到**：单文件 `clang -shared` 不会自动链接 libm，`.so` 的
   DT_NEEDED 里没有 libm.so，Android 的 linker 不做全局符号查找。加 `-lm`。
4. **调试信息**：logcat 在测试机上读不到（`adb logcat -d` 恒为空），
   所以 `LiquifyServer` 把 `native=0/1` 和加载错误写进
   `/data/data/lator.forsai/files/liquify_status.txt`。

Java 实现保留为 fallback（`System.loadLibrary` 失败时用），两条路径
逐字节一致（见上一节对比测试）。

构建产物：`lib/arm64-v8a/libliquify.so`（12.7 KB），随 APK 一起签名安装。

## 交互式液化：叠加窗 + 拖动笔刷（2026-11-22 01:45）

`C:\wt_ui` 触发后（`wt_ui.txt` = `<mode> <radius> <strength>`）：

```
UI: 1600x1600 doc, paper 528x528 at 974,145, mode=forward radius=120 strength=0.80
UI: down at doc 230,318
UI: move doc 255,342 ... 848,936
UI: up
UI: committed 1600x1600 as a new layer
```

- 用一个 `WS_EX_LAYERED` 弹出窗精确盖住画纸（位置 = 画布子窗口客户区中心 ± 文档尺寸×缩放），
  把文档像素按最近邻缩放进 32bpp DIB，`UpdateLayeredWindow` 显示；
- 笔刷每步只 `lq_apply` + `lq_flush` 脏 tile，再把脏矩形重绘到叠加层——
  120px 笔刷在 1600×1600 上每步只有几毫秒；
- 提交：把当前文档图放剪贴板 → **先把焦点还给 SAI** → Ctrl+V → 新图层（图层3）。

### 输入路径的坑

- Winlator 的触摸转鼠标对 Wine 的**按键状态不可见**：`adb input swipe` 时
  `GetAsyncKeyState(VK_LBUTTON)` 一直是 0（SAI 自己也不落笔，因为笔桥只在
  压力>0 时才合成左键）。
- 所以叠加层的笔刷改成**轮询全局光标 + 按键状态**（`GetCursorPos` +
  `GetAsyncKeyState`），不依赖窗口消息；测试时用 `wt_mouse.txt` 注入
  `mouse_event` 笔画驱动，实测 `UI: down/move/up` 正常。
- 真机用笔时，PenBridge 会在压力>0 时合成 `ACTION_BUTTON_PRESS`，这条路径
  会给 Wine 真正的按键状态——**待用笔实测**。
- 提交/取消另外暴露 `wt_ui_done` / `wt_ui_cancel` 文件，方便无键盘测试
  （回车/Esc 的 `GetAsyncKeyState` 在 adb keyevent 下同样看不到）。

当前限制：叠加窗里没有工具栏（模式/半径靠 `wt_ui.txt`），没有 undo，
每次提交都是新图层（非破坏性，可在 SAI 里手动合并/隐藏原层）。

### 叠加层里的 undo 与切模式（2026-11-22 01:50）

- `C:\wt_ui_undo`：`lq_undo()` 回滚到本笔开始时的快照并整屏重绘；
- `C:\wt_ui_mode`：重新读 `wt_ui.txt`，切换模式/半径/强度，并销毁当前场
  （下一次落笔从新的位移场开始，符合 lq.c 的「一笔一场」模型）。

测试注入的稳定性问题：在叠加层存在时，`wt_mouse.txt` 的
`mouse_event` 偶尔会卡住（memscan 线程不返回，日志里既没有
"mouse stroke" 也没有 UI 的 down/move）。这是**测试注入路径**的问题，
不是产品路径（真实用笔时由 PenBridge 的 Java 侧合成按键事件）。
下轮改用「由叠加层自己按脚本路径驱动」来测，避免再依赖注入线程。

## 叠加层工具栏（2026-11-22 02:00）

叠加窗顶部加了一条 34px 的工具栏（画进同一个 layered surface，GDI 绘制后强制
alpha=255，否则 `UpdateLayeredWindow` 下整条不可见）：

```
[FWD][BLT][PNC][CW][CCW][SMO][R-][R+][OK][X]
```

- 当前模式高亮（蓝色）；
- 点击由 `WM_LBUTTONDOWN` 处理——实测 **adb tap 会送窗口消息**（`0x201 at 226,289`），
  比轮询可靠；笔刷拖动仍靠轮询光标+按键（adb 拖拽不产生按键状态）；
- 换模式/改半径会销毁当前位移场（下一笔从新场开始）。

实测：

```
UI: button 3 -> mode=3 radius=120      # 点 CW
UI: move doc 242,339 ...               # 在绿方块上拖
UI: button 8 -> committed ...          # 点 OK -> 图层3（旋涡缺口可见）
UI: button 7 -> mode=0 radius=136      # 点 R+
UI: button 9 ...                       # 点 X 关闭
```

新增 `liquify/probe/phone_doc.sh`：一键在手机上铺好 1600×1600 文档 + 测试图。

## 选区液化（2026-11-22 02:15）

实测确认了三件事：

1. **有选区时 Ctrl+C 只复制选区包围盒**：选一支笔刷形状的区域后，剪贴板里是
   `386x386`（不是 1600x1600）；
2. **选区粘贴回到原位置**：变形后 Ctrl+V，旋涡正好落在原选区位置；
3. 选区尺寸下处理极快：`386x386 twirl` 只要 **21ms**。

新流程 `C:\wt_auto_sel`（不再 Ctrl+A）：

```
CLIPREAD: 386x386 bpp=24
LQNET: mode=twirl_cw 386x386 round trip 30ms (java warp 21ms)
AUTOSEL: done (selection warped in place)
```

截图：绿方块在选区内被旋成几缕，选区外的红线保持原样——**液化被限制在选区内**。

### 顺手修掉的一个 bug

`wt_lqnet` 的触发分支先 `DeleteFileA` 再读 spec，导致**模式参数永远读不到**
（一直跑默认 forward）。改成先读后删；上面这次 twirl 就是修复后的结果。

### 当前能力对照目标

| 目标 | 状态 |
|---|---|
| 找到持久像素缓冲并验证写回 | 走的是 SAI 自己的复制/粘贴（官方通道），导出 PNG 验证过 |
| 五种模式 | 引擎逐字节等价 + 原生 arm64 库；工具栏可切换 |
| Android 端处理 | app 内 `LiquifyServer` + NDK `libliquify.so`，63–71ms/笔 |
| 多图层 | 未做（当前是拼合结果或选区） |
| 文件夹 | 未做 |
| 选区 | **已通**（限制在选区内、原位贴回） |
| 蒙版 | 未做（可用选区近似：把蒙版通道做成选区） |

## 「就地替换」与多图层边界（2026-11-22 02:10）

### 新增 MRG 按钮（合并回当前层）

工具栏改成 11 个按钮：`FWD BLT PNC CW CCW SMO MRG R- R+ OK X`。
`MRG` 打开后，OK 提交时会在 Ctrl+V 后补一个 **Ctrl+E（合并图层）**：

```
UI: committed 1600x1600 and merged down
```

对单层文档就是「就地液化」——图层数不变，内容被替换。截图里 图层2 就是合并后的
结果（绿方块被拉出下面的尾巴）。

### 持久性用「再复制一次」验证

导出 PNG 的对话框在手机上点不动（模态对话框收不到我们的点击，回车也无效），
所以改用等价证据：合并后 `Ctrl+A / Ctrl+C` 再读剪贴板——

```
CLIPREAD: 1600x1600 bpp=32
green px ~79k, bbox (100,100)-(354,472)
```

绿方块从原来的 100..355 被拉到 **y=472**（正是 forward 笔刷的方向），
说明变形确实写进了文档内容，不是只改了屏幕。

### 多图层 / 文件夹 / 蒙版的现状

- 打开 `图层(L)` 菜单确认：SAI 只有 新建/复制/删除/清除/向下转写/合并/拼合/自由变换…
  **没有「隐藏其它图层」或「只显示当前图层」**；
- 图层面板是 SAI 自绘的，没有可读的控件树，所以「逐层隔离」拿不到图层列表；
- 现实可用的语义：
  - **合层/单层**：MRG 合并 → 就地液化；
  - **多图层**：不合并 → 液化结果作为新图层叠加，其它图层不动（非破坏）；
  - **选区**：只液化选区内（已验证）；
  - **蒙版**：可把蒙版通道做成选区后用选区路径近似。

要做真正的「只液化当前图层」，需要读图层面板（自绘），下一步可以试：
从视口 DIB 里定位图层缩略图/眼睛图标，按行推断图层数与可见性。

## 热键入口：Ctrl+Alt+L（2026-11-22 02:12）

插件不再依赖触发文件：DLL 在第一次 `WTOpen` 时起一个 message-only 窗口线程，
注册全局热键 **Ctrl+Alt+L**，按一下就开叠加层。

```
HOTKEY: Ctrl+Alt+L registered, plugin ready
（按 Ctrl+Alt+L 后）
UI: 1600x1600 doc, paper 528x528 at 974,111, mode=forward radius=120 strength=0.80
```

实测：热键 → 工具栏出现 → 画一笔 → 点 OK → `UI: committed ... as a new layer`。

体积：加完这段 DLL 涨到 95,744 字节，超过 94,720 的固定上限（`PenBridge` 按字节数
校验），所以编译加了 `-fno-asynchronous-unwind-tables -fno-unwind-tables`，
压回 93,184 字节。

## 真实数位板实测通过（2026-11-22 02:20）

用户在手机上接入 **WH851-545 Pen**（`getevent -pl` 里能看到 `WH851-545 Pen` + 键盘），
在叠加层里用笔拖动画出多笔：

```
UI: down at doc 27,427
UI: move doc 188,403
UI: down at doc 209,297
UI: move doc 215,388 ... (共 38 次 move)
```

截图里绿方块被笔拖着实时液化成一团——**笔 → PenBridge 合成按键 → Wine 按键状态 →
叠加层轮询 → 实时变形**，整条链打通。

这也回答了之前的疑问：PenBridge 在压力>0 时合成的 `ACTION_BUTTON_PRESS` 会让 Wine 的
`GetAsyncKeyState(VK_LBUTTON)` 变成按下，而 adb 的 `input swipe` 不会。所以：
- **真笔**：完整支持（拖动、实时预览）；
- **adb 触摸**：只有 tap 能触发按钮/单击，拖拽不行（仅测试用）。
