# 品牌：Winlator for sai（0.3.9）

## 改动

- 应用名：`Winlator for sai`（启动器、最近任务、设置里的应用名）
- 启动图标：用户提供的 `ic_launcher.png`（192×192，白底 + 蓝色 Windows 标）

## 资源

- 源图：`android/icon/ic_launcher.png`
- 各密度图标：`android/res/mipmap-{mdpi,hdpi,xhdpi,xxhdpi,xxxhdpi}/`
  - `ic_launcher.png`：48 / 72 / 96 / 144 / 192
  - `ic_launcher_round.png`：同尺寸，圆形裁切
  - `ic_launcher_foreground.png`：108 / 162 / 216 / 324 / 432，自适应图标前景
- `app_name` 已同步到 `values`、`values-zh-rCN`、`values-zh`、`values-pt`、`values-ru`

## 构建

图标和 `app_name` 属于资源，`build-apk.ps1` 的纯 dex 替换不会生效；需要按
[docs/16](./16-winlator-chinese.md) 的 apktool 流程重建 `resources.arsc`。
仓库里的 `android/res/mipmap-*` 已经放好新图标，回编时覆盖到 `dec/res/` 即可。

## 验证

SM-X810 安装 0.3.9 后：

- 启动器显示 `Winlator for sai`；
- 图标为新的白底 + 蓝色 Windows 标；
- 包名仍是 `com.winlator`，同签名覆盖安装，容器和 SAI 数据保留。
