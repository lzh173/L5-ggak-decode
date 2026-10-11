# GGAK CADU C++ 解码器

## 功能

- 读取 224 字节 CADU 帧并校验校验和
- 解码 FM-VE、GALS-VE、ESA、HK 和 SER 数据
- 输出解码统计信息
- 生成六联 SVG 概览图

## 构建

```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

## ImGui 图形界面

项目同时提供 `ggak_gui` 图形化解码器。CMake 会通过 `FetchContent` 自动下载 Dear ImGui 和 GLFW，不需要手动复制 ImGui 源码。

在 Windows + MSYS2 UCRT64 环境中，使用 UCRT64 自带的 CMake、Ninja 和编译器，确保所有工具链来自同一个环境：

```powershell
$env:PATH = "D:\MSYS2\ucrt64\bin;D:\MSYS2\usr\bin;$env:PATH"
cmake -S . -B build-gui -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-gui
.\build-gui\ggak_gui.exe
```

首次使用前如未安装 UCRT64 工具：

```bash
/usr/bin/pacman -S --needed mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja
```

GUI 中输入 CADU 文件路径后点击“加载并解码”，可以查看校验统计以及 FM-VE、GALS-VE、ISP-2M 和 SER 数据曲线；“严格校验”会跳过校验失败的帧。

Dear ImGui GUI 同样支持 Linux。Linux 构建使用 GLFW 的 X11/Wayland 后端和 OpenGL3，需要安装系统 OpenGL、X11/Wayland 开发包。GitHub Actions 会在 Windows 和 Ubuntu 上分别构建命令行解码器与 GUI，并上传两个平台的构建产物。

当前 GUI 不是 Android 目标：它依赖 GLFW 桌面窗口、OpenGL3 和 Windows 文件选择器。Android 需要新增 Android 原生或 SDL 后端、触摸交互、文件访问权限和 Android 应用工程，后续可以作为独立移植目标加入。

## 使用

```powershell
.\build\ggak_decode.exe input.cadu -o overview.svg
```

未指定 `-o` 时，程序在输入文件旁生成 `input.cadu.svg`。

## 图表选项

```powershell
# 同时生成 PNG
.\build\ggak_decode.exe input.cadu --png

# 指定 SVG 和 PNG 输出路径
.\build\ggak_decode.exe input.cadu -o overview.svg --png overview.png

# 使用中文图表文字
.\build\ggak_decode.exe input.cadu --chinese

# 对折线数据进行 7 点中心移动平均
.\build\ggak_decode.exe input.cadu --smooth 7

# 严格校验，仅解码校验通过的 CADU 帧
.\build\ggak_decode.exe input.cadu --strict
```

`--smooth` 未指定窗口时默认使用 5 点移动平均。平滑仅作用于折线图，不修改 ESA 热图和原始解码数据。

默认模式下，校验失败的帧仍会参与解码，但会单独统计。启用 `--strict` 后，校验失败的帧会被丢弃，不进入填充帧判断、源标识识别或仪器数据解码；总帧数和校验失败数仍会保留在统计信息中。

启用 `--png` 并成功生成 PNG 后，程序会交互式询问是否删除 SVG。输入 `y` 或 `Y` 删除，直接回车或其他输入则保留 SVG。

## 仪器说明

| 仪器 | 英文描述 | 中文翻译 |
| --- | --- | --- |
| [GGAK-E/SKIF-6](https://space.oscar.wmo.int/instruments/view/ggak_e_skif_6) | Corpuscular radiation spectrometer | 粒子辐射谱仪 |
| [GGAK-E/GALS-E](https://space.oscar.wmo.int/instruments/view/ggak_e_gals_e) | Detector of galactic cosmic rays | 银河宇宙线探测器 |
| [GGAK-E/ISP-2M](https://space.oscar.wmo.int/instruments/view/ggak_e_isp_2m) | Solar constant sensor | 太阳常数传感器 |
| [GGAK-E/VUSS-E](https://space.oscar.wmo.int/instruments/view/ggak_e_vuss_e) | Solar UV radiation sensor | 太阳紫外辐射传感器 |
| [GGAK-E/FM-E](https://space.oscar.wmo.int/instruments/view/ggak_e_fm_e) | Magnetometer instrument | 磁强计 |
| [GGAK-E/DIR-E](https://space.oscar.wmo.int/instruments/view/ggak_e_dir_e) | Solar X-ray radiation flux sensor | 太阳 X 射线辐射通量传感器 |
| [GGAK-E/SKL-E](https://space.oscar.wmo.int/instruments/view/ggak_e_skl_e) | Solar cosmic rays spectrometer | 太阳宇宙线谱仪 |

使用 `--chinese` 时，当前已解码的 FM-E、GALS-E、SKIF-6 和 ISP-2M 数据面板会显示对应中文说明。VUSS-E、DIR-E 和 SKL-E 尚无对应的数据解码分支，因此仅在本说明表中列出。

## 许可证

本项目采用 [GNU Affero General Public License v3.0](LICENSE) 许可证。

## GGAK-L4 初步支持

当前解码器可以识别 GGAK-L4 的源编号 `0x32`，并将 `0x33/0x77` 作为 L4 空闲帧处理。L4 样本中的 `0x40` 粒子计数和 `0x70` 磁场帧暂时复用已验证的 GGAK 数据布局。

L4 的 `0x0C/0x1C`、`0x2C/0x3C` 和 `0x50/0x5C` 已进入“L4 原始诊断”页面，但字段和物理单位尚未确认。页面展示的是排除填充值后的 payload 字节均值，仅用于协议分析，不代表仪器物理量。L4 目录中已确认存在 SKL-E，但其光谱包格式仍需更多样本或协议资料验证。
