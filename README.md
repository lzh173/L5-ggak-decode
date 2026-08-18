# GGAK CADU C++ 解码器

根据 `t03-all.py` 实现的 C++17 GGAK CADU 解码器。

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
```

`--smooth` 未指定窗口时默认使用 5 点移动平均。平滑仅作用于折线图，不修改 ESA 热图和原始解码数据。

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
