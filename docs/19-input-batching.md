# 笔输入批处理（借鉴 SAI Pen for Android 0.3.16）

参考实现：
<https://github.com/ametrien/Paint-Tool-SAI-pen-pressure-Android>

## 核心

Android 会把一帧内的笔样本合进一个 `MotionEvent` 的 **history**。旧版
`PenBridge` 只取当前样本，history 被丢掉；光标本来就是每个事件移一次，
所以慢的不是光标，而是**笔迹点变稀**。

0.3.16 起：

1. `handleEvent()` 先遍历 `getHistorySize()`，把每个历史样本的
   `getHistoricalX/Y/Pressure` 通过 UDP 发给 `wintab32.dll`；
2. 历史样本 **不移动光标**，只更新内部坐标；
3. 当前样本仍只 `moveCursorTo()` 一次；
4. 笔的 MOVE/HOVER 样本逐个进 `CRIT` 队列（上限 512），不再只保留最新点；
5. 手指路径保持原来的合并逻辑，避免两指手势产生多余笔画。

## 为什么这样不卡

每次 `injectPointerMove()` 都要走：

```text
X server -> Wine -> wineserver -> SAI 消息循环
```

而 X server/Wine/SAI 都在 Box64 下模拟。参考实现实测：每个样本都移光标
会在 Galaxy Tab S6 Lite 上产生 **2.5 万~3 万次/秒上下文切换**，5 个线程
抢 2 个大核。

SAI 的笔迹点从 WinTab DLL 拿，不依赖鼠标移动，所以：

- 光标：每个 `MotionEvent` 一次；
- 压感/坐标：history 全采样，一条不丢。

UDP 发送在独立线程，UI 线程只入队，不阻塞。

## 卡顿时的稀疏修正（0.3.18）

输入批处理之后，如果 SAI 本身跟不上，DLL 的 conflation 会在积压超过 3 包时
开始丢中间点，线条就变稀。0.3.18：

- `wintab32.c` 的 `POST_WINDOW` 从 3 提到 **240**（ring 256），积压时优先保留
  点，而不是优先压延迟；
- `PenBridge` 的笔样本队列上限从 512 提到 **4096**，卡顿时不丢历史点；
- 只有积压超过 96 包才重新合并，避免低延迟路径完全失效。

## 大核绑定（0.3.18）

新增 `CoreTuner`：

- 读 `/sys/devices/system/cpu/cpu*/cpu_capacity`，低于最快核 60% 的算慢核；
- 每秒读 `/proc/<sai-pid>/task/*/stat` 的 utime/stime；
- 只累计笔尖接触期间（`PenBridge.isDrawingRecently()`）的 CPU；
- 按负载挑真正干活的线程，总负载不超过快核数 × 0.9，避免全塞进 2 个大核；
- 用 `taskset -pc <mask> <tid>` 绑定，失败再试 `taskset -p`；
- 每秒重绑一次，防止 Wine 在 SAI 窗口出现时重置亲和性。

日志写到 `/data/data/com.winlator/files/pb_perf.log`。如果设备没有 `taskset`
或没有大小核信息，会自动跳过，不影响画笔。

## 还没做的

- 悬停静止时每 50 ms 补一个 pressure 0 保持笔刷光标。


## 长线自适应抽稀（0.3.20）

0.3.19 把窗口放到 240 后，长线 + 慢帧时积压会让 SAI 更卡。0.3.20 改成：

- DLL `POST_WINDOW`：240 -> **96**；
- 笔队列上限 4096 -> **2048**；
- 队列积压时按**空间距离**抽稀，而不是按时间乱丢：
  - 积压 > 256：只收离上一个入队点 >= 2px 的样本；
  - 积压 > 512：>= 4px；
  - 积压 > 1024：>= 6px；
  - 积压 < 256：全收，不抽稀。

丢的是“几乎重合的点”，拐点和长线的形状保留，长线卡顿会明显减轻。


## 跳掉原生 TouchpadView（0.3.21）

参考实现最关键的一步之前漏了：笔事件在 `onTouchEvent` 里直接消费并
`return true`，**原生 TouchpadView 不再处理同一个 MotionEvent**。

我们之前是 `handleTouch()` 处理完返回 false，原生代码又跑一遍：再移一次
光标、再走手指/滚动逻辑。长线时这是双份 X server -> Wine -> SAI 开销。

0.3.21：

- 笔事件 `handleTouch()` 返回 true，smali 提前 return；
- 点击/拖动由 `syncPenClick()` 合成：
  - ACTION_DOWN 按下左键；
  - ACTION_MOVE 不再重复移光标（`handleEvent` 已移一次）；
  - ACTION_UP/CANCEL 松开；
- 手指路径不变，仍然走手势和原生逻辑。


## 大核绑定默认关闭（0.3.24）

实测怀疑 `CoreTuner` 把 SAI 的 11 个线程全塞到 4 个大核，反而造成排队。
0.3.24 起默认**不绑定**，交给 Android 调度器：

- 默认：不启动 CoreTuner；
- 想再试验：在 app files 下建空文件 `saipen-cores-on`；
- 关闭时 `pb_perf.log` 会写 `core tuning off by default`。

如果确认大核绑定是元凶，后面可以只绑最忙的 1~2 个线程，而不是全部。

## 0.3.25：回滚到更顺的输入路径

实测 0.3.16~0.3.24 的“全 history + 大窗口 + 大核绑定”反而比 0.3.15 更卡。
0.3.25 回到 0.3.15 的输入策略：

- 不发 history，只发最新点（约 166 Hz 合并）；
- DLL `POST_WINDOW` 回到 3；
- 不跳过原生 TouchpadView；
- 不做大核绑定；
- 保留 0.3.15 之后的通用 WinTab / 32+64 位 DLL / 容器英文修复。

结论：SAI 在 Box64 下，**少发包、让 SAI 自己插值**比“全采样”更顺。

## 0.3.26：回到优化路径，但更激进地丢点

0.3.25 的完全回滚反而更不顺，所以恢复 0.3.24 的输入优化，并把抽稀提前：

| 队列积压 | 只保留离上一个点 ≥ |
|---|---|
| > 64 | 2 px |
| > 128 | 4 px |
| > 256 | 7 px |
| > 512 | 10 px |

- 队列上限 2048 -> 1024；
- 落笔关键队列 512 -> 256；
- 大核绑定默认关闭；
- DLL `POST_WINDOW` 96。

目标：长线/慢帧时更早开始丢“几乎重合”的点，优先保顺滑，而不是保密度。

## 0.3.27：SAI 单独走回 0.3.10 的 DLL 路径

0.3.11 之后的通用 WinTab 改动可能让 SAI 选择了不同的路径。0.3.27 的 DLL
按宿主进程分流：

- 宿主是 `sai2.exe`：使用 0.3.10 的 WTInfo 子集、固定 36 字节报文、
  `POST_WINDOW=3`，也就是当时顺滑的那条路；
- 其他应用（FireAlpaca/Krita）：继续使用完整 WTInfo、按 `lcPktData`
  动态构造报文和 polling。

这样不用在“SAI 顺滑”和“通用压感”之间二选一。

## 大核绑定结论：负面

在 SM-X810 / Snapdragon 8 Gen 2 上实测，打开 `saipen-cores-on` 后长线更卡。
`CoreTuner` 把 11 个 SAI 线程全放到 4 个大核，反而造成排队。保持默认关闭，
不再启用。
