# 画布像素捕获与写回（实测，2026-11-21 22:20）

这一页只写**实际验证过**的事实。没验证的会明确标出「未验证」。

## 一、SAI 怎么把画布画到屏幕

在 `wintab32.dll` 里 hook `sai2.exe` 的 GDI IAT 之后观察到的调用分布：

| 调用 | 情况 |
|---|---|
| `BitBlt` | **0 次** |
| `StretchDIBits` | **0 次** |
| `CreateDIBSection` | 只有控件尺寸（如 231×259），没有画布 |
| `SetDIBitsToDevice` | **唯一**的绘制通道，画布、工具栏、面板全走它 |

画布那一次调用的形态（1600×1600 文档，50% / 75% / 100% 缩放都一样）：

```
SetDIBitsToDevice 800x800 at (180,73) start=0 lines=2560 bits=0x8D70000
  dst sflChildWindow "新建画布1 (50%)"
```

- 源 DIB：**3584×2560、32bpp、stride 14336、BI_RGB**
- `start=0`，`lines=2560` 覆盖整张 DIB
- 贴图目标：标题带 `(xx%)` 的画布子窗口

含义：**这张 DIB 是“屏幕空间（视口空间）”的合成缓冲**，不是文档缓冲。
一个 DIB 像素 = 一个屏幕像素；100% 缩放时才与文档像素 1:1。

用 `wt_canvas.bmp` 转储验证过内容正确（黑底 + 灰色画框 + 白色画纸 + 笔画）。

## 二、写回：能生效

探针在 `SetDIBitsToDevice` 返回前，往 DIB 中心写 320×320 品红（BGRA=FF00FFFF）：

- 转储出的 BMP 里出现品红方块 → **字节确实被改写**
- 屏幕上的画布直接显示品红 → **修改会随这次 blit 一起上屏**
- 反复触发重绘（多次 blit）、改缩放（100%→75%）之后**仍然在**

结论：SAI **不是每帧从图层重新合成**，只重绘脏区；只要它不认为那块脏，写进去的像素就一直留在画面上。

## 三、还没定的：这算不算“文档内容”

写进 DIB 的像素会不会：

1. 被“另存为”写进文件？
2. 被撤销/重做、画笔落笔等文档级操作冲掉？
3. 被复制/滤镜等内部读取操作读到？

这三条决定架构，**必须先验证第 1 条**（另存为 PNG 后读回来看像素）。
验证方法：写品红 → 文件→另存为 `C:\users\xuser\save1.png` → 拉回来看。

## 四、图层像素不在这块缓冲里（已验证）

在画布有内容（1600×1600 渐变图 + 笔画）时，从 DIB 取一段 64 像素宽、变化最剧烈的
特征串（256 字节），扫描整个进程的可读私有内存：

```
done: 237 regions (135 >=4MB), 1 hits
HIT 0x96BD2A0 region=0x8D70000 size=35840 KB
```

唯一命中就是 DIB 自己（`0x8D70000` 正是 `bits`）。

也就是说：**图层像素不是以「和屏幕一致的 32bpp 平铺」形式存在的**。
可能原因（均未验证）：分块 tile 存储、每通道 16 位、预乘 alpha、压缩/量化的历史数据、
或者干脆是 SAI 自己的图块格式。

进程里只有 3 个超过 4MB 的私有可写区域：9MB ×1、35MB ×2（其中一个是 DIB）。
1600×1600×4 = 10.24MB，没有对应的区域 → 更说明不是裸的 32bpp。

## 五、探针的接口（`liquify/probe/wintab32_probe.c`）

在 `wintab32.dll` 里加一整套**纯内存、触发式**的开发探针。**热路径绝不写文件**——
早期版本在 `SetDIBitsToDevice` 里直接 `fflush`，笔画期间把 SAI 拖死到整机 ANR。

| 开关文件（相对 `drive_c`） | 作用 |
|---|---|
| `wt_canvas_capture` | 加载时安装 GDI IAT hook（存在才装） |
| `wt_write` | 下一次画布 blit 时往 DIB 写 320×320 品红，落盘 BMP + 日志 |
| `wt_memscan` | 从下一次画布 blit 取特征串，扫描全进程私有内存 |
| `wt_pattern.bin` + `wt_memscan` | 用指定字节串做模式搜索（判定像素格式用） |
| `wt_dump.txt`（内容 `"<hexaddr> <len>"`） | 转储任意内存区域到 `wt_dump.bin` |

输出：`wt_gdi.log`、`wt_canvas.bmp`、`wt_memscan.log`、`wt_dump.bin`。

### 部署注意（踩过的坑）

- APK 内置了 DLL：`PenBridge.ensureBridgeFiles()` 在启动容器时按 **文件长度** 判断是否覆盖。
  调试版必须把 DLL 补齐到**正好 94720 字节**（补 0 即可），否则一启动就被换回发行版。
- 同时 `wt_bridge_ver.txt` 要写成 `wintab-2026-11-21-10`，否则 `force=true` 一样覆盖。
- 用 `am force-stop` 会丢掉未保存文档；SAI 的恢复数据在
  `AppData/Local/SYSTEMAX Software Development/SAIv2/hisdata/<时间戳>.N/`。

## 六、液化引擎（`liquify/engine/`）

与捕获/写回解耦的纯 C99 引擎，同一份代码要能编进 Wine DLL，也能在 Mac 上跑单测。

模型：**逆位移场 + 分块重建**

- 一笔开始时复制整面（这份拷贝同时是 undo 源和重采样源）
- 每个笔刷步只累加逆位移场（`int16`，1/16 像素，按 64×64 tile 惰性分配）
- `lq_flush()` 只重建脏 tile，对同一份位移场**幂等**
- 平滑是破坏性的（没有位移可表达），所以同步更新拷贝

实测（M 系列 Mac，单线程）：

```
forward dirty bbox 83,119 .. 241,201
changed=8389  changed_outside_100px=0
idempotent=1
undo_restores=1
big canvas 3584x2560: stroke_begin + 40 steps + flush = 18.2 ms
```

六种模式：`forward / bloat / pinch / twirl_cw / twirl_ccw / smooth`。
