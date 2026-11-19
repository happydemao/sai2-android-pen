# 贡献指南

## 这个仓库接受什么

- 在其他机型 / 其他笔上的实测结果（带 `C:\wt_status.txt` 与 `C:\wtlog.txt` 片段）。
- `PenBridge` 里"用压力自己判断落笔"的判据改进（现在的阈值是经验值，见 `docs/07-status-and-limits.md`）。
- 坐标映射的 letterbox（现在整面拉伸，16:10 的板子配 16:9 的容器会轻微纵向变形）。
- 倾斜 / 橡皮的协议扩展（需要同时改 `bridge/wintab32.c` 的报文和 `PenBridge` 的发送端）。

## 这个仓库不接受什么

- **任何 SAI 本体、安装包、授权证书、破解或绕过方式**。PR 里出现这类内容会直接关闭。
- Winlator 的完整源码副本或 APK（走上游与 Releases）。
- 签名材料（`*.keystore` / `*.jks`）。

## 提交前

```bash
# 1) DLL 逻辑改了要能自测（wintab_core.h 是纯 C，不依赖 Win32）
cd bridge && clang -O2 -o /tmp/wtcore_test wintab_core_test.c 2>/dev/null || true

# 2) 端到端校验：真实 Java 类 → 报文 → DLL 解析逻辑
python3 tools/verify_bridge.py

# 3) 别把构建垃圾带进来
git status --short
```

## 语言

文档目前以中文为主。PR 用中文或英文都可以；改到的文档请保持同一种语言，不要中英混排。
