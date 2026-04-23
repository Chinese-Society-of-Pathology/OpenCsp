# OpenCsp

在中华医学会病理学分会和中国医师协会病理科医师分会的支持下，持续推动病理数字切片共享格式的标准化推广与应用。共享格式文件后缀统一为 .CSP，即 Chinese Society of Pathology。基于 CSP 数字病理图像数据存储格式规范，建设 CSP 格式开源仓库，推动相关技术开放共享，促进数字病理行业互联互通与协同发展。

## OpenCsp 项目目录结构

```text
OpenCsp/
├── CSP数字病理图像数据存储格式规范260422.docx   # CSP 格式规范文档
├── LICENSE                                      # 开源许可证
├── README.md                                    # 项目说明文档
├── readme.txt                                   # 本文件（项目目录架构说明）
│
├── DecodeCsp/                                   # CSP 解码模块（C 语言实现）
│   ├── build.ps1                                # PowerShell 构建脚本
│   ├── DecodeCsp.c                              # 解码器源代码
│   ├── readme.txt                               # 模块说明
│   └── sample.csp                               # 示例 CSP 文件
│
└── EncodeCsp/                                   # CSP 编码模块（C++ 实现）
    ├── CMakeLists.txt                           # CMake 构建配置
    ├── csp_writer.cpp                           # CSP 写入器实现
    ├── csp_writer.hpp                           # CSP 写入器头文件
    ├── main.cpp                                 # 主程序入口
    ├── readme.txt                               # 模块说明
    └── slide/                                   # 示例切片图像数据
        ├── label.jpeg                           # 标签图像
        ├── macro.jpeg                           # 宏观图像
        ├── thumbnail.jpeg                       # 缩略图
        ├── layer0/                              # 第 0 层（最高分辨率，1638 个切片）
        │   └── {row}_{col}.jpeg                 #   命名格式：行号_列号.jpeg
        ├── layer1/                              # 第 1 层（420 个切片）
        │   └── {row}_{col}.jpeg
        ├── layer2/                              # 第 2 层（110 个切片）
        │   └── {row}_{col}.jpeg
        ├── layer3/                              # 第 3 层（30 个切片）
        │   └── {row}_{col}.jpeg
        ├── layer4/                              # 第 4 层（9 个切片）
        │   └── {row}_{col}.jpeg
        ├── layer5/                              # 第 5 层（4 个切片）
        │   └── {row}_{col}.jpeg
        └── layer6/                              # 第 6 层（最低分辨率，1 个切片）
            └── {row}_{col}.jpeg
```

## 说明

- `slide/layer*` 目录存放多分辨率金字塔切片图像，layer0 分辨率最高，layer6 最低。
- 切片文件命名格式为 `{行号}_{列号}.jpeg`，例如 `0_0.jpeg`、`10_5.jpeg` 等。
- 各层切片总数：layer0=1638, layer1=420, layer2=110, layer3=30, layer4=9, layer5=4, layer6=1。
