#pragma once

/**
 * csp_writer.hpp
 *
 * CSP 格式写入接口（对应 openslide-vendor-csp.c 解析器的逆操作）
 *
 * 文件结构（写入顺序）：
 *   [0 ~ 127]       128 字节文件头（byte 12-13 = offset_type = 32）
 *   [128 ~]         UID_EQUIPMENT 条目
 *                   UID_ASSOCIATED_IMAGE 条目 x N（可选）
 *                   UID_MULTI_SCAN_RESULT 条目（含金字塔元数据）
 *                   UID_PIXEL_DATA 条目头（14 字节）
 *                   [像素数据区：关联图像 JPEG + 各层 tile JPEG]
 *
 * 使用示例见 main.cpp
 */

#include <cstdint>
#include <string>
#include <vector>

namespace csp {

/// 关联图像类型（与解析器保持一致）
constexpr uint8_t ASSOC_TYPE_LABEL     = 0;  // 标签图
constexpr uint8_t ASSOC_TYPE_MACRO     = 1;  // 宏观/预览图
constexpr uint8_t ASSOC_TYPE_THUMBNAIL = 2;  // 缩略图

/// 单个 tile 的输入描述
struct TileInput {
    uint32_t    pixel_x;    ///< tile 左边缘像素坐标（绝对坐标）
    uint32_t    pixel_y;    ///< tile 上边缘像素坐标（绝对坐标）
    uint32_t    width;      ///< tile 宽度（0 = 自动从 JPEG 头读取）
    uint32_t    height;     ///< tile 高度（0 = 自动从 JPEG 头读取）
    std::string jpeg_path;  ///< JPEG 文件路径
};

/// 金字塔单层（frame）的输入描述
struct FrameInput {
    uint32_t               frame_id;     ///< 帧 ID（从 0 开始递增）
    uint32_t               frame_width;  ///< 本层全图宽度（像素）
    uint32_t               frame_height; ///< 本层全图高度（像素）
    float                  frame_ratio;  ///< 本层放大倍率（如 40.0f / 20.0f）
    std::vector<TileInput> tiles;        ///< 本层所有 tile（顺序不限）
};

/// 关联图像的输入描述
struct AssocImageInput {
    uint8_t     image_type; ///< ASSOC_TYPE_LABEL / ASSOC_TYPE_MACRO / ASSOC_TYPE_THUMBNAIL
    std::string jpeg_path;  ///< JPEG 文件路径
};

/// 写入 CSP 文件所需的完整配置
struct CspWriteConfig {
    // —— 扫描仪信息 ——
    std::string manufacturer;   ///< 厂商名，如 "CSP Inc."
    std::string model_name;     ///< 型号，如 "CS-40"

    // —— 扫描参数 ——
    std::string scan_time;          ///< 扫描时间字符串，如 "2024-01-01 12:00:00"
    uint32_t    tile_width  = 256;  ///< 基础 tile 宽度
    uint32_t    tile_height = 256;  ///< 基础 tile 高度
    float       scan_ratio  = 40.f; ///< 最大放大倍率
    float       mpp         = 0.f;  ///< 每像素微米数（µm/px）
    float       downsampling_ratio = 2.f; ///< 相邻金字塔层的降采样比（2.0 = 与参考文件一致）

    // —— 金字塔层（index 0 = 最高分辨率层）——
    std::vector<FrameInput> frames;

    // —— 关联图像（可选）——
    std::vector<AssocImageInput> associated_images;
};

/**
 * 将给定配置写入 CSP 文件。
 *
 * @param output_path  输出 .csp 文件路径
 * @param config       写入配置（所有 jpeg_path 必须可读）
 *
 * @throws std::runtime_error  任何 I/O 或格式错误时抛出
 *
 * 限制：
 *   - 采用 U32 寻址模式，总像素数据不超过 4 GB
 *   - 仅支持单次扫描、单图像层（与当前 CSP 产品一致）
 *   - crc32 字段填 0（不计算）
 */
void write_csp(const std::string& output_path, const CspWriteConfig& config);

} // namespace csp
