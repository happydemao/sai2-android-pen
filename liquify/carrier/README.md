# mscms.dll 代理载体（SAI2 原生插件方法）

参考对象：桌面的 `mscms.dll`（16 MB，Rust 写的 SAI2 toolkit 运行时）。拆开看到的特征：

- **代理 DLL**：完整导出 mscms 的 182 个函数名，SAI2 导入的是系统色彩管理 DLL，
  把同名 DLL 放到 `sai2.exe` 旁边，加载器会在 SAI 第一条指令之前调用我们；
- **原生补丁引擎**：字符串里是 `sai2-pe` / `imports` / `trampolines` /
  `operand_patches` / `command_table` / `action_filters` / `host_license` 这类数据结构，
  也就是按签名在内存里定位 SAI 的函数和数据、打补丁/挂钩；
- **内置 UI**：导入 USER32/GDI32/COMCTL32/UxTheme/SHELL32/ole32，有自己的窗口；
- **插件 API**：导出 `SAI2PluginInitialize`、`SAI2PluginPrepareWithProxy`、
  `SAI2PluginInitializeWithProxy`，第三方插件链接它；
- **授权**：`sai2-license-core`、`host_license`（带 SAI 代码 RVA + 字节签名的白名单），
  这是商业组件的做法，我们不能直接复用它的二进制。

我们只借**载体方法**：本目录的 `mscms.dll` 是一个 22 KB 的 x64 代理，
182 个导出全部转发到 `mscms_orig.dll`（容器里 Wine 自带的 mscms 改名而来），
`DllMain` 里拉起我们的插件线程。这样：

- 不需要再当 WinTab 驱动（wintab32.dll 那条线可以退役）；
- 从进程启动第一刻就在 SAI 里，可以先于它的初始化做挂钩；
- 色彩管理照常工作（转发到原 DLL，不用改 SAI 的任何文件）。

## 构建

```bash
MINGW_BIN=/private/tmp/llvm-mingw/bin ./build_carrier.sh
```

## 部署到容器

```bash
SERIAL=... PKG=lator.forsai USERDIR=xuser ./deploy_carrier.sh
```

它做三件事：把 `drive_c/windows/system32/mscms.dll` 复制成 SAI 目录下的
`mscms_orig.dll`；把我们的 `mscms.dll` 放进去；清掉旧日志。启动 SAI 后
`C:\mscms_carrier.log` 里会出现 `CARRIER: attached`。

## 下一步

载体跑通后，把液化实现搬进这个 DLL：按签名定位 SAI 的图层像素缓冲（toolkit 的做法），
直接在图层上做位移场变形，不再走剪贴板——那条路的对齐问题就此消失。
