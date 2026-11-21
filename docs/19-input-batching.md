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
