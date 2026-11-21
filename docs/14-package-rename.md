# 把 Winlator 改成自己的包名（`lator.forsai`）

> 基线：`sai2-android-gh/Winlator-for-sai-0.3.27.apk`（11-21 20:21）
> 结果：`lator.forsai` 已装到 SM-X810 上，容器能起来、Wine 桌面正常、压感桥在写新路径的 `pb.log`。
> 产物：`sai2-android-gh/Winlator-for-sai-0.3.27-lator.forsai.apk`

## 一、关键：新旧包名必须**一样长**

Winlator 的 rootfs / box64 / wine / X 库里有 ~450 处编译死的
`/data/data/com.winlator/...`。只要新旧包名等长，全部可以**原位替换**，
所有偏移量、ELF 段、tar 头都不动：

```
com.winlator  (12)  ->  lator.forsai  (12)
/data/data/com.winlator/  ->  /data/data/lator.forsai/
```

这比"改任意长度包名"简单一个数量级：不需要搬字符串、不需要修 `lea`/`adrp`、
不需要重编 wine。项目里的等长替换实测覆盖了：

| 位置 | 内容 |
|---|---|
| `assets/rootfs.tzst` | 447 处：`libc.so` 链接脚本、`ld.so.cache`、fontconfig、pulse、wine 的 `ntdll.so` / `wineserver` / `nsiproxy.so`、`libxcb` 的 X socket 路径、box64 的 `PT_INTERP`… |
| `assets/box64/*.tzst` | 2 处（`PT_INTERP` + `RUNPATH`） |
| `assets/container_pattern.tzst` | 1 处（`dosdevices/z:` 的绝对软链目标） |
| `assets/rootfs_patches.tzst` | 1 处 |
| `assets/graphics_driver/*.tzst` | turnip / gladio / virgl / vortek 各 1～3 处 |
| `lib/arm64-v8a/libwinlator.so` | 1 处（`/data/data/.../cache/.vk-api-version`） |
| `lib/arm64-v8a/libvortekrenderer.so` | 3 处（vortek cache 路径） |

**注意**：`.tzst` 是 tar+zstd，但因为是等长替换，可以直接对**解压后的 tar 字节流**
做替换再重新压缩——tar 头里只有文件名，不含这个字符串，所以不会破坏结构；
软链目标存在文件内容里，也会一起被替换。

## 二、具体步骤

```bash
# 1. 反编译（复用已有的 framework）
java -jar tools/bin/apktool.jar d -f -p apk_analyze/apktool_framework \
    -o dec sai2-android-gh/Winlator-for-sai-0.3.27.apk

# 2. manifest + smali 里的包名/路径（Java 类名保持 com.winlator.* 不动！）
python3 tools/rename/apply_smali.py dec

# 3. 资产和 native 库里的等长替换
python3 tools/rename/rewrite_assets.py dec
python3 tools/rename/rewrite_assets.py dec --check   # 应输出 tzst total: 0

# 4. 打包、对齐、签名（key 与现有 Release 一致）
java -jar tools/bin/apktool.jar b dec -o raw.apk -p apk_analyze/apktool_framework
sdk/bt/android-15/zipalign -P 16 -f 4 raw.apk aligned.apk
sdk/bt/android-15/apksigner sign --ks keys/debug.keystore \
    --ks-pass pass:android --key-pass pass:android \
    --ks-key-alias androiddebugkey --out Winlator-0.3.27-lator.forsai.apk aligned.apk
```

`apply_smali.py` 只做三件事：

1. `AndroidManifest.xml`：`package="lator.forsai"`，两个 provider authority
   （`FileProvider`、`androidx-startup`）；
2. 所有 smali 里 `/data/data/com.winlator/` → `/data/data/lator.forsai/`
   （`Container.DEFAULT_DRIVES`、`WineUtils`、`ContainerDetailFragment`、
   `PenBridge`、`CoreTuner`）；
3. `FileUtils` 里的 `"com.winlator.FileProvider"`。

**千万不要**全局替换 `com.winlator`：manifest 的 `android:name="com.winlator.MainActivity"`、
布局里的 `com.winlator.widget.*`、PenBridge 反射用的 `com.winlator.xserver.*`
都是 **Java 类名**，必须保留。

`com.winlator.action.ENSURE_FOREGROUND` 也可以保留：只有 ForegroundService
内部用到，通知 PendingIntent 与过滤器是同一个常量，改成新值反而要两处一起动。

## 三、我在设备上验证到的

- 包名 `lator.forsai`，版本 `11.2 (33)`，安装正常、启动正常；
- 新容器自动创建：`e:` 盘指向 `/data/data/lator.forsai/storage`，`z:` 是相对链；
- `wineserver / services.exe / explorer.exe` 全部以新 uid 运行，Wine 桌面（Computer 窗口、
  任务栏、时钟）正常显示；
- PenBridge / CoreTuner 在写 `/data/data/lator.forsai/files/pb.log`、`pb_perf.log`
  （说明 smali 路径改对了，压感链路跟着新包走）。

## 四、如果一定要用不等长包名

那就只能做真正的路径重定位，工作量完全不是一个量级（我试过 `com.winlator.sai2specialized`）：

- `box64`：可以只改 `PT_INTERP`（把新字符串追加到文件尾、`p_offset` 指过去，
  内核不校验 `p_vaddr`）——这部分好办，见 `tools/rename/patch_interp.py`；
- x86-64 wine 的 `ntdll.so` / `wineserver` / `nsiproxy.so`：需要把字符串搬进
  两个 `PT_LOAD` 之间的全零 padding，再把唯一的 `lea reg,[rip+disp32]` 指过去
  （`tools/rename/grow_string.py`）。**不能**截断/覆盖 section header（box64 的
  dlopen 会读它），也**不能**让新字符串落在 BSS 覆盖范围里（会把 wine 的 BSS 变量
  用 section header 垃圾盖掉，wineserver 崩在 `alloc_fd_object`）；
- AArch64 的 `libxcb.so`：重写 `adrp`+`add`，注意 `add` 的目标寄存器可能不是
  `adrp` 那个（`tools/rename/a64_grow.py`）；
- 就算这些都做了，当时还卡在 `winex11.drv` 加载成功但 user32 报
  `no driver could be loaded`（连 `connect()` 都没发出）。等长方案一次性绕开了这类
  "还有没找到的写死路径"的问题。

## 五、换包名后的副作用

- 新包 = 全新数据目录：旧 `com.winlator` 的容器、SAI、注册表都不跟过来；
- 两个包可以共存（签名相同也没关系，包名不同）；
- 内置组件（box64/DXVK/Turnip/wineprefix 模板）会按新路径重新解压一份，
  首次启动约 1 分钟、占 ~2 GB。


## 0.3.28 发布说明

GitHub Release `v0.3.28` 的 APK 已经是新包名 **`lator.forsai`**，
文件名 `Winlator-for-sai-0.3.28-lator.forsai.apk`。

注意：

- 新包数据目录是 `/data/data/lator.forsai/`，旧 `com.winlator` 的数据不迁移；
- 两个包可共存，但不要同时启动 SAI，否则会出现两套容器/两条输入路径；
- 安装新包后如果容器是第一次启动，需要重新解压 rootfs，耗时约 1 分钟。

## 0.3.29：默认值调整

在 `lator.forsai` 的 `classes.dex` 上：

1. **关闭“捕获外接鼠标指针”默认值**
   - `SettingsFragment.smali`：`capture_pointer_on_external_mouse` 的
     `getBoolean` 默认值 `0x1` -> `0x0`
   - `XServerDisplayActivity.smali`：读取同一 preference 前
     `const/4 v4, 0x0`

2. **容器默认分辨率 = 屏幕分辨率**
   - `Container.<init>(I)` 的默认 `"1280x720"` 改为
     `AppUtils.getScreenWidth()/getScreenHeight()` 的长边 x 短边。

3. **DPI 按实际屏幕**
   - `ScreenInfo.getWidthInMillimeters()` / `getHeightInMillimeters()`
     不再用 `width/10`（固定 254 DPI），改为
     `pixels * 25.4 / DisplayMetrics.xdpi`（或 `ydpi`）。
   - SM-X810 上报 `xdpi≈274.6`、`ydpi≈275.5`，所以 Wine 屏幕约 275 DPI，
     而不是原来的 254。

4. 当前容器 `screenSize` 从 `2560x1660` 修正为 `2560x1600`。

## 0.3.30：新容器默认分辨率的真正入口

0.3.29 只改了 `Container.<init>()`，但新建容器界面在
`ContainerDetailFragment.onCreateView()` 里写死了：

```smali
:cond_3
const-string v0, "1280x720"
```

所以 UI 仍然把新容器填成 1280x720。0.3.30 改成在 `onCreateView` 里用
`AppUtils.getScreenWidth()/getScreenHeight()` 的长边 x 短边，和
`Container` 构造函数保持一致。

当前容器 `screenSize` 已设为 `2560x1600`，启动后保持不被覆盖。
