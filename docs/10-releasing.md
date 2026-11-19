# 发布清单（GitHub Release）

仓库本体只放源码、文档和 185 KB 的预编译 DLL。**APK 永远走 Releases，不进 git**。

## 0. 仓库里不该出现的东西

`.gitignore` 已经挡住，但发布前再确认一遍：

| 不能提交 | 原因 |
|---|---|
| `.stage/` | 构建垃圾（十几个 150 MB 的中间 APK），且**含 `debug.keystore`——私钥泄露等于别人能冒充你发布** |
| `android-sdk/`、`tools-jdk/`、`.gradle-home/`、`.android-home/` | 4–8 GB 的环境副本 |
| `winlator-build/` | Winlator 源码副本（LGPL，应该由上游分发） |
| `*.apk` | 159 MB，超 GitHub 单文件 100 MB 硬限制 |
| `NotoSansCJK-Regular.ttc` | 31 MB 字体，用 OFL 授权的官方下载地址代替 |
| SAI 本体 / 安装器 / `.slc` 授权证书 | 商业软件，**任何情况下都不入库** |

## 1. 发布前自检

```bash
# 工作区干净？
git status --short

# 库里最大的 10 个文件（都应在 1 MB 以下，dist/*.dll 约 185 KB）
git ls-files -z | xargs -0 ls -l | sort -k5 -n | tail -10

# 有没有误传签名材料 / 破解相关字样
git ls-files | grep -Ei 'keystore|\.jks|\.p12|\.apk$|crack|keygen'

# 文档里还有没有本机路径
grep -rn "/Users/\|C:\\\\Users\\\\" --include=*.md . || echo clean
```

## 1.5 首次推送到 GitHub

```bash
# 在 GitHub 建空仓库（不要勾 README / .gitignore / LICENSE，本地都有了）
git remote add origin git@github.com:<你的账号>/sai2-android.git
git push -u origin main
```

推送前最后确认一次：

```bash
git ls-files | wc -l          # 应该是 69 个文件
git count-objects -vH | grep size-pack   # 打包后应在 1 MB 上下
```

> **exFAT 上的两个坑**（本项目就在一块 exFAT 移动硬盘上整理，都踩过）：
>
> 1. exFAT 不支持 Unix 权限位，`git status` 会把所有文件报成 "modified" →
>    `git config core.filemode false`。
> 2. macOS 会在 exFAT 上给文件生成 `._xxx` 伴生文件（AppleDouble）。若它出现在
>    `.git/objects/pack/` 里，`git gc` 会把 `._pack-*.idx` 当成真的索引读，然后报
>    `non-monotonic index` 并中止。所以这台盘上的仓库设了 `git config gc.auto 0`
>    （不自动打包，保持松散对象）。
>
> 想彻底避免：把仓库克隆到 APFS/HFS+ 盘再推送，移动硬盘只当资料存档。

## 2. 打 tag

```bash
git tag -a v0.1.0 -m "SAI2 on Android: pen pressure via Winlator"
git push origin main --tags
```

版本号建议：`0.1.x` 阶段表示"能画、能压感，倾斜/橡皮未做"。

## 3. 建 Release

附件两个文件：

| 文件 | 说明 |
|---|---|
| `Winlator-11.2-SAIPen.apk` | 159 MB |
| `Winlator-11.2-SAIPen.apk.idsig` | 1.2 MB，`apksigner` 的 v4 签名文件 |

> 单文件上限：Release 附件 2 GB，够用；**普通 git 仓库只到 100 MB**，所以别提交 APK。
> 想让用户直接 `curl -L` 下载，用 `https://github.com/<user>/<repo>/releases/latest/download/Winlator-11.2-SAIPen.apk`。

## 4. Release 说明模板

```markdown
## 这是什么

Winlator 11.2 的**非官方修改版**，内置数位板压感发送端，用于在 Android 上跑
PaintTool SAI Ver.2 并拿到真实笔压。**不含 SAI 本体**，你需要自己的合法副本。

## 装之前

- 包名仍是 `com.winlator`，签名与官方不同 → **必须先卸载官方 Winlator**
  （**容器和里面的东西会一起没**，先备份 `Android/data/com.winlator/files/rootfs/home/`）。
- 首次启动若弹 16 KB 对齐/调试签名提示，点"确定"即可。

## 改了什么

- 只替换 `classes.dex`、新增 `classes2.dex`；资源、assets、35 个原生库全是原版字节。
- 两处 `TouchpadView` 入口调用 `PenBridge`（压感采样 + 笔点击合成），逻辑全在 Java 里。
- 详细差异与 smali 补丁：[android/](../android/)、[docs/04-patching-winlator.md](../docs/04-patching-winlator.md)。

## 配套文件（仓库 dist/）

`wintab32.dll`、`wt_pmax.txt`、`1-压感覆盖.reg`、`2-中文字体.reg`。

## 已知问题

- SAI 每会话第一笔可能弹 `FileError(161)`，非致命。
- 倾斜、橡皮未转发。
- 首次安装需在 SAI 里打开 *Other → Options → Pen Tablet → Use WinTab API* 并重启 SAI。

## 许可

本 APK 是 Winlator 的修改版。Winlator 与 Wine 为 LGPL-2.1-or-later，Box64 为 MIT：
源码请到各自上游获取（https://github.com/brunodev85/winlator、
https://github.com/ptitSeb/box64）。本仓库的修改以 GPL-3.0 发布，见 NOTICE 与 LICENSE。
```

## 5. LGPL 合规要点（Winlator / Wine）

分发修改过的 LGPL 二进制时至少要给到：

1. **明示这是修改版**，以及修改了什么（上面"改了什么"一节）；
2. **给出对应源码的位置**——Winlator 上游 + 本仓库的差分（`android/` 与本文档）；
3. 不限制接收方反向工程/替换该库的权利（不要在 EULA 里写相反的话）。

## 6. 发布后

- 在仓库 Settings 里打开 Issues，模板里要求贴 `C:\wt_status.txt`、`C:\wtlog.txt` 和机型。
- 若收到 DMCA / 法务投诉：先下架 Release 附件，再核对内容——本仓库不含 SAI 本体，
  通常问题出在 APK 里的 Winlator/Wine 部分，按 LGPL 补上源码说明即可。
