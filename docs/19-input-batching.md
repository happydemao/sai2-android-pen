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

## 还没做的

参考实现还会：

- 用 `/sys/.../cpu_capacity` 找慢核；
- 统计落笔期间 SAI 各线程 CPU，把真正干活的线程 `taskset` 到快核；
- 悬停静止时每 50 ms 补一个 pressure 0 保持笔刷光标。

这些属于下一步优化，当前先落地输入批处理。
