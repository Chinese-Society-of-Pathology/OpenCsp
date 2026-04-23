/*
 * main.cpp
 *
 * 从 slide/ 目录读取图像数据，生成 output.csp 文件。
 *
 * 目录结构（相对于本可执行文件位置）：
 *   ../slide/
 *       label.jpeg       992×992，关联图像-标签图
 *       macro.jpeg       1872×992，关联图像-宏观图
 *       thumbnail.jpeg   256×237，关联图像-缩略图
 *       layer0/          最高分辨率金字塔层（20x），tile 命名 row_col.jpeg
 *       layer1/          10x
 *       layer2/          5x
 *       layer3/          2.5x
 *       layer4/          1.25x
 *       layer5/          0.625x
 *
 * 扫描参数：
 *   mpp              = 0.18 µm/pixel
 *   scan_time        = "2026-04-22 10:24:33"
 *   scan_ratio       = 20x
 *   downsampling     = 0.5（相邻层 1:2 降采样）
 *   tile_size        = 256×256
 */

#include "csp_writer.hpp"

#include <algorithm>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <regex>
#include <stdexcept>
#include <string>
#include <sys/stat.h>

// ================================================================
// 从 JPEG SOF 读取尺寸（轻量实现，无需外部库）
// ================================================================
static bool read_jpeg_dims(const std::string& path, uint32_t& w, uint32_t& h) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    uint8_t b0, b1;
    f.read(reinterpret_cast<char*>(&b0), 1);
    f.read(reinterpret_cast<char*>(&b1), 1);
    if (b0 != 0xFF || b1 != 0xD8) return false;
    while (f.good()) {
        uint8_t ff, mk;
        f.read(reinterpret_cast<char*>(&ff), 1);
        if (!f.good()) break;
        if (ff != 0xFF) continue;
        f.read(reinterpret_cast<char*>(&mk), 1);
        if (mk == 0xFF) { f.seekg(-1, std::ios::cur); continue; }
        if (mk == 0xD9) break;
        uint8_t lh, ll;
        f.read(reinterpret_cast<char*>(&lh), 1);
        f.read(reinterpret_cast<char*>(&ll), 1);
        const uint16_t seg_len = static_cast<uint16_t>((lh << 8) | ll);
        const bool is_sof = (mk >= 0xC0 && mk <= 0xC3) ||
                            (mk >= 0xC5 && mk <= 0xC7) ||
                            (mk >= 0xC9 && mk <= 0xCB) ||
                            (mk >= 0xCD && mk <= 0xCF);
        if (is_sof) {
            uint8_t prec, hh, hl, wh, wl;
            f.read(reinterpret_cast<char*>(&prec), 1);
            f.read(reinterpret_cast<char*>(&hh), 1);
            f.read(reinterpret_cast<char*>(&hl), 1);
            f.read(reinterpret_cast<char*>(&wh), 1);
            f.read(reinterpret_cast<char*>(&wl), 1);
            h = static_cast<uint32_t>((hh << 8) | hl);
            w = static_cast<uint32_t>((wh << 8) | wl);
            return true;
        }
        const int64_t skip = static_cast<int64_t>(seg_len) - 2;
        if (skip > 0) f.seekg(skip, std::ios::cur);
    }
    return false;
}

static bool path_exists(const std::string& path) {
    struct stat st {};
    return stat(path.c_str(), &st) == 0;
}

static std::string path_join(const std::string& lhs, const std::string& rhs) {
    if (lhs.empty()) return rhs;
    const char tail = lhs.back();
    if (tail == '/' || tail == '\\') return lhs + rhs;
    return lhs + "/" + rhs;
}

static std::string path_filename(const std::string& path) {
    const size_t pos = path.find_last_of("/\\");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

// ================================================================
// 扫描单个 layerN 目录，构建 FrameInput
// frame_w / frame_h：本层的全图实际像素尺寸（由调用者传入）
// ================================================================
static csp::FrameInput scan_layer_dir(
    const std::string& dir,
    uint32_t frame_id,
    float    frame_ratio,
    uint32_t frame_w,
    uint32_t frame_h,
    uint32_t default_tile_w,
    uint32_t default_tile_h)
{
    // 瓦片文件名格式：<row>_<col>.jpeg（不区分大小写）
    const std::regex name_re(R"(^(\d+)_(\d+)\.jpe?g$)", std::regex::icase);

    struct Entry { uint32_t row, col; std::string path; };
    std::vector<Entry> entries;

    DIR* dp = opendir(dir.c_str());
    if (!dp)
        throw std::runtime_error("Cannot open directory: " + dir);

    for (dirent* de = readdir(dp); de != nullptr; de = readdir(dp)) {
        const std::string fname = de->d_name;
        if (fname == "." || fname == "..") continue;
        std::smatch m;
        if (!std::regex_match(fname, m, name_re)) continue;
        const uint32_t row = static_cast<uint32_t>(std::stoul(m[1]));
        const uint32_t col = static_cast<uint32_t>(std::stoul(m[2]));
        entries.push_back({row, col, path_join(dir, fname)});
    }
    closedir(dp);

    if (entries.empty())
        throw std::runtime_error("No JPEG tiles found in: " + dir);

    // 按 (row, col) 排序
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        return a.row != b.row ? a.row < b.row : a.col < b.col;
    });

    csp::FrameInput frame;
    frame.frame_id     = frame_id;
    frame.frame_ratio  = frame_ratio;
    frame.frame_width  = frame_w;
    frame.frame_height = frame_h;

    frame.tiles.reserve(entries.size());
    for (const auto& e : entries) {
        csp::TileInput ti;
        // tile 坐标：像素坐标 = 网格索引 × tile 尺寸
        ti.pixel_x   = e.col * default_tile_w;
        ti.pixel_y   = e.row * default_tile_h;
        // 宽高设为 0，让 csp_writer 从 JPEG SOF 中读取实际尺寸
        ti.width     = 0;
        ti.height    = 0;
        ti.jpeg_path = e.path;
        frame.tiles.push_back(std::move(ti));
    }

    return frame;
}

// ================================================================
// main
// ================================================================
int main() {
    // 路径相对于可执行文件所在目录（build/）
    const std::string slide_dir = "./slide";
    const std::string out_csp = "./output.csp";

    // ---- 扫描参数 ----
    constexpr uint32_t TILE_W       = 256;
    constexpr uint32_t TILE_H       = 256;
    constexpr float    SCAN_RATIO   = 20.0f;  // 最大倍率 20x
    constexpr float    MPP          = 0.18f;  // µm/pixel
    constexpr float    DS_RATIO     = 0.5f;   // 每层缩小一半（frame 尺寸计算）
    constexpr int      NUM_LAYERS   = 7;

    csp::CspWriteConfig cfg;
    // manufacturer 使用 GBK 编码的原始字节（与真实扫描仪参考文件一致）
    // 深圳市生强科技有限公司 in GBK: C9 EE DB DA CA D0 C9 FA C7 BF BF C6 BC BC D3 D0 CF DE B9 AB CB BE
    cfg.manufacturer       = std::string("\xC9\xEE\xDB\xDA\xCA\xD0\xC9\xFA\xC7\xBF\xBF\xC6\xBC\xBC\xD3\xD0\xCF\xDE\xB9\xAB\xCB\xBE", 22);
    cfg.model_name         = std::string("\x01\x00", 2);  // 与参考文件一致
    cfg.scan_time          = "2026-04-22 10:24:33";
    cfg.tile_width         = TILE_W;
    cfg.tile_height        = TILE_H;
    cfg.scan_ratio         = SCAN_RATIO;
    cfg.mpp                = MPP;
    cfg.downsampling_ratio = 2.0f;  // 与参考文件一致（表示每层降采样 2 倍）

    // ---- 关联图像 ----
    cfg.associated_images = {
        { csp::ASSOC_TYPE_LABEL,     path_join(slide_dir, "label.jpeg")     },
        { csp::ASSOC_TYPE_MACRO,     path_join(slide_dir, "macro.jpeg")     },
        { csp::ASSOC_TYPE_THUMBNAIL, path_join(slide_dir, "thumbnail.jpeg") },
    };

    // ---- 金字塔各层 ----
    // 先扫描 layer0 的实际图像尺寸（通过读取角落 tile 的 JPEG 头 + tile 数量推算）
    // 实际尺寸 = (tile数量-1)*tile_size + 最后一个 tile 的实际尺寸
    uint32_t level0_w = 0, level0_h = 0;
    {
        const std::string layer0_dir = path_join(slide_dir, "layer0");
        if (!path_exists(layer0_dir))
            throw std::runtime_error("layer0 not found");

        const std::regex name_re(R"(^(\d+)_(\d+)\.jpe?g$)", std::regex::icase);
        uint32_t max_row = 0, max_col = 0;
        std::string last_row_path, last_col_path;

        DIR* dp = opendir(layer0_dir.c_str());
        if (!dp)
            throw std::runtime_error("Cannot open directory: " + layer0_dir);

        for (dirent* de = readdir(dp); de != nullptr; de = readdir(dp)) {
            const std::string fname = de->d_name;
            if (fname == "." || fname == "..") continue;
            std::smatch m;
            if (!std::regex_match(fname, m, name_re)) continue;
            uint32_t row = static_cast<uint32_t>(std::stoul(m[1]));
            uint32_t col = static_cast<uint32_t>(std::stoul(m[2]));
            if (row > max_row || (row == max_row && col == 0)) {
                max_row = row;
                last_row_path = path_join(layer0_dir, fname);
            }
            if (col > max_col || (col == max_col && row == 0)) {
                max_col = col;
                last_col_path = path_join(layer0_dir, fname);
            }
        }
        closedir(dp);
        // 找最大行号的第一列 tile（确定 height），最大列号的第一行 tile（确定 width）
        // 简化：直接读最大行列对应的 tile
        uint32_t last_tile_w = TILE_W, last_tile_h = TILE_H;
        std::string corner_path = path_join(
            layer0_dir, std::to_string(max_row) + "_" + std::to_string(max_col) + ".jpeg");
        if (!path_exists(corner_path))
            corner_path = path_join(
                layer0_dir, std::to_string(max_row) + "_" + std::to_string(max_col) + ".jpg");
        if (path_exists(corner_path))
            read_jpeg_dims(corner_path, last_tile_w, last_tile_h);

        level0_w = max_col * TILE_W + last_tile_w;
        level0_h = max_row * TILE_H + last_tile_h;
        std::cout << "Level0 actual dimensions: " << level0_w << "x" << level0_h << "\n";
    }

    float ratio = SCAN_RATIO;
    uint32_t cur_w = level0_w, cur_h = level0_h;
    for (int i = 0; i < NUM_LAYERS; ++i) {
        const std::string layer_dir = path_join(slide_dir, "layer" + std::to_string(i));
        if (!path_exists(layer_dir)) {
            std::cerr << "Warning: " << layer_dir << " not found, skipping.\n";
            ratio *= DS_RATIO;
            cur_w = (cur_w + 1) / 2;
            cur_h = (cur_h + 1) / 2;
            continue;
        }

        std::cout << "Scanning " << path_filename(layer_dir) << "  (ratio=" << ratio << "x)...\n";

        csp::FrameInput frame;
        try {
            frame = scan_layer_dir(layer_dir, i, ratio,
                                   cur_w, cur_h,
                                   TILE_W, TILE_H);
        } catch (const std::runtime_error& e) {
            const std::string msg = e.what();
            if (msg.find("No JPEG tiles found in: ") == 0) {
                std::cerr << "Warning: " << msg << ", skipping.\n";
                ratio *= DS_RATIO;
                cur_w = (cur_w + 1) / 2;
                cur_h = (cur_h + 1) / 2;
                continue;
            }
            throw;
        }

        std::cout << "  tiles=" << frame.tiles.size()
                  << "  frame=" << frame.frame_width << "x" << frame.frame_height << "\n";

        cfg.frames.push_back(std::move(frame));
        ratio *= DS_RATIO;
        cur_w = (cur_w + 1) / 2;
        cur_h = (cur_h + 1) / 2;
    }

    if (cfg.frames.empty()) {
        std::cerr << "Error: no pyramid layers found under " << slide_dir << "\n";
        return 1;
    }

    // ---- 写入 CSP 文件 ----
    std::cout << "\nWriting " << out_csp << " ...\n";
    try {
        csp::write_csp(out_csp, cfg);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
