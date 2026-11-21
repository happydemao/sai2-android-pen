# Winlator 主界面汉化（0.3.8）

> 目标：让 Winlator 11.2 的主界面、容器编辑、设置页显示简体中文，不影响 SAI 容器数据。

## 为什么原来一直是英文

两个原因叠加：

1. **没有中文资源**：Winlator 11.2 只带 `values`、`values-pt`、`values-ru` 等，
   没有 `values-zh-rCN`。
2. **用户设置强制了 English**：`com.winlator_preferences.xml` 里 `lc_index=0`。
   `LocaleHelper.supportedLocales` 只有 `en_US / pt_BR / ru_RU`，索引 0 就是 English，
   所以即使系统语言是中文，启动时也会被 `setSystemLocale()` 切回英文。

## 改动

### 1. 中文资源

`android/res/values-zh-rCN/strings.xml`、`arrays.xml`
（同时复制了一份 `values-zh`，兼容只报 `zh` 的设备）：

- 318 条主界面/设置/对话框/文件管理器/Toast 文案；
- 下拉项：键盘/鼠标/手柄、标准/XInput、MIDI 通道、语言列表；
- 语言列表新增第 4 项「简体中文」。

### 2. LocaleHelper 增加 zh_CN

`smali/com/winlator/core/LocaleHelper.smali` 的静态数组由：

```smali
.registers 3
const-string v0, "pt_BR"
const-string v1, "ru_RU"
const-string v2, "en_US"
filled-new-array {v2, v0, v1}, [Ljava/lang/String;
```

改为：

```smali
.registers 4
const-string v0, "pt_BR"
const-string v1, "ru_RU"
const-string v2, "en_US"
const-string v3, "zh_CN"
filled-new-array {v2, v0, v1, v3}, [Ljava/lang/String;
```

这样：

- `getLocaleIndex()` 在中文系统下返回 3，设置页语言显示「简体中文」；
- `setSystemLocale()` 会切到 `zh`，加载 `values-zh-rCN`；
- Wine 的开始菜单/资源管理器由于系统字体也能显示中文。

### 3. 不要给 Wine 设置 LC_ALL=zh_CN.UTF-8

`setEnvVars()` 原本会按 `supportedLocales` 给容器设置 `LC_ALL`。加入 zh_CN 后它会把
`LC_ALL` 设成 `zh_CN.UTF-8`，实测 SAI 会把中文文件名解码错：

```text
色板1.saiswat  ->  h?2f??1.saiswat
无法读取设置文件 (code=0xA1)
```

所以 `LocaleHelper.setEnvVars()` 增加了一处判断：语言名以 `zh` 开头时跳过，
继续落到 `en_US.UTF-8`。Android 界面仍然是中文，Wine/SAI 的文件编码保持原来的 en_US。

对应补丁也在 `build-apk.ps1` 的 `LocaleHelper env locale` 步骤里。

`android/build-apk.ps1` 已加入同一处 smali 补丁（步骤 `3c`）。

### 4. 资源需要重新编译

`build-apk.ps1` 只替换 dex，不重建 `resources.arsc`。所以这一版的中文界面是用
apktool 完整反编译/回编资源得到的；代码补丁仍与 `build-apk.ps1` 保持一致。

仓库里保留了 `android/res/values-zh-rCN` 和 `values-zh`，以后可以：

```bash
apktool d -f -o dec base.apk
cp -r android/res/values-zh* dec/res/
# 再按 build-apk.ps1 的步骤打 smali 补丁
apktool b dec -o localized-unsigned.apk
# 替换 classes2.dex、zipalign、apksigner
```

## 安装 / 切换语言

- 已安装旧版 / 已把语言选成 English 的设备：安装 0.3.8 后进入
  **设置 → 语言 → 简体中文**；
- 也可以直接改偏好（需 root）：

```bash
adb shell su -c "sed -i 's/name=\"lc_index\" value=\"0\"/name=\"lc_index\" value=\"3\"/' \
  /data/data/com.winlator/shared_prefs/com.winlator_preferences.xml"
```

然后强制停止并重启 Winlator。

## 实测

SM-X810 + Winlator 11.2（0.3.8）：

- 主界面标题「容器」、新建按钮、容器菜单均为中文；
- 容器列表、设置页、语言列表出现「简体中文」；
- Wine 开始菜单显示「起点」（Start）；
- 容器和 SAI 数据保持原样（同签名覆盖安装）。

## 修正：容器内不汉化（0.3.12）

早期补丁里 `if-eqz` / `if-nez` 写反了：语言名以 `zh` 开头时反而**没有跳到跳过分支**，
继续把 `LC_ALL` 设成了 `zh_CN.UTF-8`。结果是 Android 界面中文，但 Wine 容器里的
文件管理器/开始菜单也变成中文，并且中文文件名在 Wine 的文件管理器里显示为乱码。

0.3.12 改为：

```smali
if-nez v7, :cond_0   # 以 zh 开头 -> 跳到 increment，跳过 LC_ALL 设置
```

现在：

- Android 侧 Winlator 界面仍可中文；
- 容器/Wine 保持 `en_US.UTF-8` 或未设置，开始菜单为 `Start`、文件管理器为英文；
- 容器内中文文件名的乱码问题消失。
