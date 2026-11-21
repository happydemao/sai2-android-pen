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
