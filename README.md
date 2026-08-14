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

## 许可证

本项目采用 [GNU Affero General Public License v3.0](LICENSE) 许可证。
