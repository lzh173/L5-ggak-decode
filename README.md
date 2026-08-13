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

## 许可证

本项目采用 [GNU Affero General Public License v3.0](LICENSE) 许可证。
