DecodeCsp 项目说明
===================

1. 项目目标
-----------
本项目提供一个独立的 CSP 解析程序（不依赖 OpenSlide），支持：
1) 解析 CSP 元数据与金字塔层/瓦片索引
2) 按 layer + 瓦片位置读取原始 JPEG 瓦片
3) 输出文件中的数字字典（UID -> 数值数组）


2. 文件结构
-----------
- DecodeCsp.c
  主代码文件（C++17），包含：
  - CSP 常量定义（UID、DT、offset_type）
  - 数据结构定义（CspTileInfo / CspFrameInfo / CspScanConfig 等）
  - 核心读取器类 CspReader
  - 命令行入口 main

- build.ps1
  PowerShell 编译脚本。默认使用：
  D:\mingw64\bin\g++.exe

- sample.csp
  示例 CSP 文件，用于本地验证解析与瓦片导出。


3. 代码结构（核心）
-------------------
命名空间：csp

核心类：CspReader
- Open(path, error)
  打开并解析 CSP，构建内存索引。

- LayerCount() / Layers()
  返回主图像金字塔层信息。

- ReadTileJpeg(layer, tile_col, tile_row, out, error)
  按层号和瓦片网格坐标读取 JPEG 字节。

- ReadTileJpegByOrigin(layer, tile_x, tile_y, out, error)
  按瓦片左上角像素坐标读取 JPEG 字节。

- GetNumericDictionary()
  返回数字字典（UID -> uint64 数值数组）。

解析流程简述：
1) 读取全文件到内存
2) 从 128 字节后按条目头循环解析
3) 递归构建 Sequence 子节点树
4) 解析设备信息、扫描配置、图像层级、瓦片索引
5) 记录 pixel_data 起始偏移，用于后续瓦片提取


4. 编译环境
-----------
已验证环境：
- OS: Windows 11
- Shell: PowerShell
- Compiler: g++.exe (MinGW-W64) 8.1.0
- C++ Standard: C++17

默认编译器路径：
- D:\mingw64\bin\g++.exe

如你的编译器路径不同，可在执行脚本时通过参数覆盖（见下文）。


5. 如何编译
-----------
在项目目录执行：

PowerShell:
  .\build.ps1

指定编译器路径：
  .\build.ps1 -CompilerPath "D:\mingw64\bin\g++.exe"

指定输入输出文件名：
  .\build.ps1 -Source "DecodeCsp.c" -Output "DecodeCsp.exe"

编译后立即跑示例：
  .\build.ps1 -RunAfterBuild

如果 PowerShell 限制脚本执行，可先执行（仅当前会话生效）：
  Set-ExecutionPolicy -Scope Process Bypass


6. 如何运行
-----------
1) 仅解析并打印信息：
  .\DecodeCsp.exe .\sample.csp

2) 读取并导出指定瓦片（layer/col/row）：
  .\DecodeCsp.exe .\sample.csp 0 0 0 out_tile_0_0_0.jpg

命令格式：
  DecodeCsp <csp_path>
  DecodeCsp <csp_path> <layer> <tile_col> <tile_row> [out.jpg]


7. 输出说明
-----------
程序运行后会打印：
- 厂商、型号、扫描时间
- TileSize、LayerCount、PixelDataOffset
- 每层尺寸和瓦片数量
- NumericDictionary 键数量及部分示例值


8. 常见问题
-----------
1) 中文显示乱码
  Windows 控制台编码可能不是 UTF-8，可执行：
  chcp 65001

2) 提示找不到 g++.exe
  检查编译器路径，或使用 build.ps1 的 -CompilerPath 参数覆盖。

3) 提示 layer/tile 越界
  先运行不带瓦片参数的命令，确认层数和瓦片分布后再读取。
