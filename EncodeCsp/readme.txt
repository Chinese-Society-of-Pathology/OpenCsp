EncodeCsp 项目说明（readme.txt）
生成时间：2026-04-23 16:14:42

1. 项目用途
本项目用于将 slide 目录中的金字塔 JPEG 瓦片及关联图像（label/macro/thumbnail）封装为 CSP 文件。
默认输出文件为 output.csp。

2. 代码结构
- main.cpp
  负责程序入口与业务编排：
  1) 扫描 slide/layerN 目录中的 tile 文件（命名规则 row_col.jpeg/jpg）。
  2) 读取 layer0 角点 tile，推导 level0 全图尺寸。
  3) 按 downsampling 规则计算各层 frame 尺寸与倍率。
  4) 组装 CspWriteConfig（扫描参数、关联图像、frame/tile 列表）。
  5) 调用 csp::write_csp(...) 生成 output.csp。

- csp_writer.hpp
  对外接口与数据结构定义：
  TileInput / FrameInput / AssocImageInput / CspWriteConfig，
  以及 write_csp(const std::string&, const CspWriteConfig&) 声明。

- csp_writer.cpp
  CSP 编码核心实现：
  1) 构建各 UID 节点与二进制头。
  2) 编排帧、tile、关联图像元数据。
  3) 写入像素数据段（JPEG 原始字节）。
  4) 输出完整 CSP 文件。

- CMakeLists.txt
  CMake 构建入口，设置 C++17，目标程序 csp_writer。

3. 编译环境（本机实际）
- OS: Microsoft Windows 11 Home China (10.0.26200, 64-bit)
- Shell: PowerShell
- Compiler: g++.exe (x86_64-posix-seh-rev0, Built by MinGW-W64 project) 8.1.0
- Compiler Path: D:\mingw64\bin\g++.exe
- C++ Standard: C++17

4. 编译与运行
4.1 使用 g++ 直接编译（当前可用方式）
  g++ -std=c++17 -Wall -Wextra main.cpp csp_writer.cpp -o csp_writer.exe

4.2 运行
  .\\csp_writer.exe

4.3 运行产物
- 可执行文件: csp_writer.exe
- 输出文件: output.csp

5. 项目目录结构（含子目录内容，自动采集）
说明：以下为生成 readme.txt 时的完整目录树。

Folder PATH listing
Volume serial number is 5C43-3CBB
C:.
|   CMakeLists.txt
|   csp_writer.cpp
|   csp_writer.hpp
|   main.cpp
|   output.csp
|   readme.txt
|   
\---slide
    |   label.jpeg
    |   macro.jpeg
    |   readme.txt
    |   thumbnail.jpeg
    |   
    +---layer0 
    |
    +---layer1
    |              
    +---layer3
    |       
    +---layer4
    |       
    +---layer5
    |       
    \---layer6
           
            
