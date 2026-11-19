# 第三方组件与许可

本仓库自己的代码与文档以 **GPL-3.0** 发布（见 [LICENSE](./LICENSE) 与 [NOTICE](./NOTICE)）。
下表列出所有第三方来源，以及发布时需要注意的义务。

## 1. 仓库内直接包含的

| 内容 | 位置 | 来源 | 许可 | 义务 |
|---|---|---|---|---|
| `wintab32.c` / `wintab_core.h` / `wintab32.def` | `bridge/` | [ametrien/Paint-Tool-SAI-pen-pressure-macOS-fix](https://github.com/ametrien/Paint-Tool-SAI-pen-pressure-macOS-fix)（已修改） | GPL-3.0 + §7 附加条款 | 保留 `NOTICE`；保留署名与链接；注明"已修改"、不得冒充原作 |
| `wintab32.dll` / `wtprobe.exe` | `dist/` | 由上面三个文件交叉编译 | 同上 | 同上（并提供对应源码 = 本仓库 `bridge/`） |
| `SaiPressureBridge.java` | `android/` | 上述项目的 macOS 助手的安卓移植 | 同上 | 同上 |
| `PenBridge.java` / `PenSender.java` / `ProbeSender.java` | `android/`、`tools/java/` | 本项目原创 | GPL-3.0 | 随仓库 |
| 文档、脚本、`tools/*.py`、`tools/*.ps1` | `docs/`、`android/scripts/`、`tools/` | 本项目原创 | GPL-3.0 | — |

> ⚠️ **历史提示**：本仓库早期文档曾把这些代码标为 MIT。那是错的——上游 0.3.17 起改用
> GPL-3.0，只有 0.3.16 及更早的版本才是 MIT。现在文档已全部更正为 GPL-3.0。

## 2. 发布 Release APK 时会一并分发的

`Winlator-11.2-SAIPen.apk` 是基于 Winlator 的重打包产物，内含以下自由软件。**只把它作为
Release 附件分发时，仍需满足各自的许可**：

| 组件 | 许可 | 你需要做的 |
|---|---|---|
| [Winlator](https://github.com/brunodev85/winlator) | LGPL-2.1-or-later | 提供对应源码获取方式；说明本 APK 是修改版 |
| Wine | LGPL-2.1-or-later | 同上（源码随 Winlator 构建体系） |
| [Box64](https://github.com/ptitSeb/box64) | MIT | 保留版权声明 |
| 其余原生库 | 见 Winlator 仓库 | 同上 |

因此 Release 说明里必须写明：本 APK 是 Winlator 11.2 的**非官方修改版**（只替换了
`classes.dex`、新增 `classes2.dex`），修改内容见 `android/`，Winlator/Wine/Box64 的源码请到
各自上游获取。**不要**在仓库里直接提交 APK。

## 3. 不在仓库里、但教程会提到的

| 内容 | 许可 | 处理方式 |
|---|---|---|
| **PaintTool SAI Ver.2** | 商业软件（SYSTEMAX） | **绝不入库**。用户自带合法副本；保存功能需自行购买授权 |
| NotoSansCJK 字体（`NotoSansCJK-Regular.ttc`） | SIL OFL-1.1 | 不入库，从 [Google Fonts](https://github.com/notofonts/noto-cjk) 自行下载 |
| 第三方重打包的 SAI 安装器 | 授权不明 / 可能侵权 | **不入库、不链接、不在文档中引用** |

## 4. 商标

PaintTool SAI 与 SAI 为 SYSTEMAX Software Development 的商标/产品名。本项目与 SYSTEMAX
无关联，未获其背书。
