> **0.3.0 起不再需要手动放这些文件**：压感桥和中文字体已内置在 APK 的容器模板里，
> 新建容器自动具备。这里的文件只用于**旧版 APK**、或给**升级前就存在的容器**手动补桥。

# dist/ —— 要放进容器的文件

这些是给最终用户的二进制/配置，源码在 `../bridge/`（GPL-3.0，见 `../NOTICE`）。

| 文件 | 放到容器里的位置 | 作用 |
|---|---|---|
| `wintab32.dll` | `C:\windows\system32\wintab32.dll` | 压感桥本体。SAI 通过它拿到笔压；它自己从 UDP `47800` 收样本 |
| `wt_pmax.txt` | `C:\wt_pmax.txt` | 压力上限（内容一行 `4095`）。**不复制也能画**，但会退回内置默认 1023，压力轴只剩 1/4 |
| `1-压感覆盖.reg` | 双击导入（容器内 regedit） | `wintab32=native,builtin` + `WT_DEBUG=1`，让 Wine 优先加载我们的 DLL |
| `2-中文字体.reg` | 双击导入（容器内 regedit） | 把 Tahoma 等替换成 **Noto Sans CJK SC**，修方块字。换别的字体就先改文件里的族名再导入 |
| `wtprobe.exe` | 任意目录，**和 `wintab32.dll` 同目录** | 桌面端探针：不碰 SAI 就能验"报文 → DLL"通不通 |

## 先在不碰 SAI 的情况下验通（10 分钟）

在 Windows 上把 `wintab32.dll` + `wtprobe.exe` 放同一目录，写一个 `C:\wt_pmax.txt`（`4095`），
运行 `wtprobe.exe`，再用仓库的 `../tools/sai_pen_test.py` 打样本。窗口里的压力开始跳，就说明
报文格式对了。详见 `../docs/03-wintab-dll-protocol.md`。

> `wtprobe.exe` 的源码在上游项目的 `wintab-src/wtprobe.c`，本项目未改动。
