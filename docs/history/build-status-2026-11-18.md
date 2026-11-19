# 补丁 APK 构建状态（2026-11-18 00:35）

## 已完成

| 步骤 | 结果 |
|---|---|
| 拉取 winlator-app 源码（主仓子模块为空，从 GitHub 下载 154 MB ZIP） | OK |
| 在工作区搭最小 SDK（platform-35 + build-tools 36） | OK |
| JDK 21（Gradle 8.14.5 不支持 JDK 25） | OK |
| 关闭 externalNativeBuild，改用从**已装 APK 抽出**的 35 个预编译 .so | OK（CMake 4.1.2 配 NDK 时挂死，必须绕开） |
| 去掉 R8、关 useLegacyPackaging 告警 | OK |
| Java 补丁（penmode 开关 + ACTION_DOWN 即按住左键） | OK，dex 里已确认含 "penmode" |
| 构建 assembleDebug | BUILD SUCCESSFUL |
| 并存安装为 com.winlator.pen（原版 com.winlator 未动） | OK |

## 当前阻塞

补丁版启动容器时**永远停在 "Starting up..."**，且：

- 补丁版（uid 10311）**没有产生任何 wine 进程**
- 它的 rootfs 完整（997 MB，box64 / wine 都在）
- 原版（uid 10310）容器照常运行

结论：问题在**重打包后的 App 启动阶段**（原生库/资源打包方式），不在 Java 补丁本身。

## 待验证的疑点（按可能性）

1. **原生库打包差异**：原版 APK 的 .so 是 NDK r27+ 产物（16 KB 对齐），我们是从 APK **解出来再打进 zip**，
   对齐/压缩方式可能变了；系统弹过 "不符合 16 KB 对齐要求"。
2. **重复的 libzstd-jni**：AGP 警告 "2 files found for lib/zstd-jni"，可能取错。
3. **资源（assets）路径/压缩差异**。

## 下一步（建议）

1. 抓应用自己的日志：db logcat -s Winlator:* AndroidRuntime:E，或让 Winlator 的 LogView 显示
2. 若确认是 .so 打包：改用 zipalign -P 16 重新对齐 + 重签，或在 build.gradle 里显式
   ndroid:extractNativeLibs=false 与 useLegacyPackaging=false 组合
3. 备选：直接替换原版 APK 的 classes.dex（最保真，但会覆盖原版、丢容器数据）
