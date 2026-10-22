/*
 * csp_writer.cpp
 *
 * CSP 格式写入实现（对应 openslide-vendor-csp.c 解析器的对称编码器）
 *
 * 文件内存布局（U64 寻址模式）：
 *   [0  ~ 127]  : 128 字节文件头（byte 12-13 = 64，即 U64 寻址模式）
 *   [128 ~]     : UID_EQUIPMENT 条目
 *                 UID_ASSOCIATED_IMAGE 条目 × N
 *                 UID_MULTI_SCAN_RESULT 条目（含完整金字塔元数据）
 *                 UID_PIXEL_DATA 条目头（22 字节，value_length = 像素区总字节数）
 *                 [像素数据区：关联图像 JPEG + 各层 tile JPEG]
 *
 * 条目头（U64 模式，22 字节）：
 *   [0-1]  MID          uint16 LE
 *   [2-3]  EID          uint16 LE  ;  UID = (MID << 16) | EID
 *   [4-5]  DT           uint16 LE  ;  0x000E = DT_SEQUENCE，其余为叶节点
 *   [6-13] value_num    uint64 LE  ;  DT_SEQ 时 = 直接子节点数
 *   [14-21] value_length uint64 LE  ;  value 区字节数
 *
 * UID_TILE_INFO value 布局（36 字节，小端）：
 *   [0-3]   width      uint32
 *   [4-7]   height     uint32
 *   [8-15]  data_offset uint64   ; 相对于 pixel_file_offset
 *   [16-23] data_length uint64   ; JPEG 字节长度（解析器读取 8 字节）
 *   [24-27] x          uint32
 *   [28-31] y          uint32
 *   [32-35] crc32      uint32
 */

#include "csp_writer.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace csp {

// ================================================================
// UID 常量（镜像解析器定义）
// ================================================================
static constexpr uint32_t UID_EQUIPMENT          = 0x00010001;
static constexpr uint32_t UID_MANUFACTURER       = 0x00010002;
static constexpr uint32_t UID_MODEL_NAME         = 0x00010003;
static constexpr uint32_t UID_ASSOCIATED_IMAGE   = 0x00020001;
static constexpr uint32_t UID_IMAGE_TYPE         = 0x00020002;
static constexpr uint32_t UID_IMAGE_WIDTH        = 0x00020003;
static constexpr uint32_t UID_IMAGE_HEIGHT       = 0x00020004;
static constexpr uint32_t UID_IMAGE_DATA_OFFSET  = 0x00020005;
static constexpr uint32_t UID_IMAGE_DATA_LENGTH  = 0x00020006;
static constexpr uint32_t UID_PIXEL_DATA         = 0x00030001;
static constexpr uint32_t UID_MULTI_SCAN_RESULT  = 0x00050001;
static constexpr uint32_t UID_SCAN_RESULT        = 0x00050002;
static constexpr uint32_t UID_SCAN_CONFIGURATION = 0x00040001;
static constexpr uint32_t UID_SCAN_TIME          = 0x00040003;
static constexpr uint32_t UID_SLICE_BASIC_WIDTH  = 0x00040007;
static constexpr uint32_t UID_SLICE_BASIC_HEIGHT = 0x00040008;
static constexpr uint32_t UID_SCAN_RATIO         = 0x00040009;
static constexpr uint32_t UID_SCAN_MPP           = 0x0004000A;
static constexpr uint32_t UID_DOWNSAMPLING_RATIO = 0x00060002;
static constexpr uint32_t UID_MULTI_IMAGE_INFO   = 0x00020009;
static constexpr uint32_t UID_IMAGE_INFO         = 0x0002000A;
static constexpr uint32_t UID_IMAGE_ID           = 0x0002000B;
static constexpr uint32_t UID_MULTI_FRAME_INFO   = 0x0002001E;
static constexpr uint32_t UID_FRAME_INFO         = 0x0002001F;
static constexpr uint32_t UID_FRMAE_ID           = 0x00020020;
static constexpr uint32_t UID_FRMAE_RATIO        = 0x00020021;
static constexpr uint32_t UID_FRAME_WIDTH        = 0x00020022;
static constexpr uint32_t UID_FRAME_HEIGHT       = 0x00020023;
static constexpr uint32_t UID_MULTI_TILE_INFO    = 0x00020024;
static constexpr uint32_t UID_TILE_INFO          = 0x00020025;

static constexpr uint16_t DT_SEQ  = 0x000E; // 容器节点
static constexpr uint16_t DT_LEAF = 0x0001; // 叶节点（解析器只检查 DT_SEQ）

// ================================================================
// 字节缓冲区工具
// ================================================================
using Buf = std::vector<uint8_t>;

static void put_u16(Buf& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v));
    b.push_back(static_cast<uint8_t>(v >> 8));
}

static void put_u32(Buf& b, uint32_t v) {
    for (int i = 0; i < 4; ++i)
        b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

static void put_u64(Buf& b, uint64_t v) {
    for (int i = 0; i < 8; ++i)
        b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

static void put_f32(Buf& b, float v) {
    uint32_t tmp;
    std::memcpy(&tmp, &v, 4);
    put_u32(b, tmp);
}

static void cat(Buf& dst, const Buf& src) {
    dst.insert(dst.end(), src.begin(), src.end());
}

// ================================================================
// 条目头（U64 模式，22 字节）
// ================================================================
static void emit_hdr(Buf& b, uint32_t uid, uint16_t dt,
                     uint64_t vnum, uint64_t vlen) {
    put_u16(b, static_cast<uint16_t>(uid >> 16));    // MID
    put_u16(b, static_cast<uint16_t>(uid & 0xFFFF)); // EID
    put_u16(b, dt);
    put_u64(b, vnum);
    put_u64(b, vlen);
}

// 写入叶节点条目（header + raw value）
static void leaf(Buf& dst, uint32_t uid, const Buf& value) {
    emit_hdr(dst, uid, DT_LEAF, 1u, static_cast<uint64_t>(value.size()));
    cat(dst, value);
}

// 写入容器节点条目（header + 已序列化的子节点）
static void seq(Buf& dst, uint32_t uid, uint64_t nchildren, const Buf& children) {
    emit_hdr(dst, uid, DT_SEQ, nchildren, static_cast<uint64_t>(children.size()));
    cat(dst, children);
}

// ================================================================
// Value 工厂函数
// ================================================================
static Buf V_u8 (uint8_t  v) { return {v}; }
static Buf V_u32(uint32_t v) { Buf b; put_u32(b, v); return b; }
static Buf V_u64(uint64_t v) { Buf b; put_u64(b, v); return b; }
static Buf V_f32(float    v) { Buf b; put_f32(b, v); return b; }
// 字符串写入时包含 null 终止符，确保 parser 侧的 g_strdup / strlen 能正确截断
static Buf V_str(const std::string& s) {
    Buf b(s.begin(), s.end());
    b.push_back(0u); // null terminator
    return b;
}// 原始字节写入（不添加 null 终止符，用于需要与参考文件字节精确匹配的字段）
static Buf V_raw(const std::string& s) { return Buf(s.begin(), s.end()); }
// ================================================================
// UID_TILE_INFO 二进制布局（36 字节）
// 对应 parse_tile_info 读取顺序：
//   width(4) height(4) data_offset(8) data_length(8) x(4) y(4) crc32(4)
// ================================================================
static Buf make_tile_blob(uint32_t w, uint32_t h,
                          uint64_t data_offset, uint64_t data_length,
                          uint32_t x, uint32_t y) {
    Buf b;
    b.reserve(36);
    put_u32(b, w);
    put_u32(b, h);
    put_u64(b, data_offset);
    put_u64(b, data_length);
    put_u32(b, x);
    put_u32(b, y);
    put_u32(b, 0u); // crc32 = 0（不计算）
    return b;
}

// ================================================================
// JPEG SOF 维度解析（无需外部库，仅扫描文件头）
// ================================================================
static bool jpeg_dims(const std::string& path, uint32_t& w, uint32_t& h) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;

    uint8_t b0, b1;
    f.read(reinterpret_cast<char*>(&b0), 1);
    f.read(reinterpret_cast<char*>(&b1), 1);
    if (b0 != 0xFF || b1 != 0xD8) return false; // 非 JPEG SOI

    while (f.good()) {
        uint8_t ff, mk;
        f.read(reinterpret_cast<char*>(&ff), 1);
        if (!f.good()) break;
        if (ff != 0xFF) continue;
        f.read(reinterpret_cast<char*>(&mk), 1);
        if (mk == 0xFF) { f.seekg(-1, std::ios::cur); continue; } // 填充字节
        if (mk == 0xD9) break; // EOI

        // 读取段长度（大端，含自身 2 字节）
        uint8_t lh, ll;
        f.read(reinterpret_cast<char*>(&lh), 1);
        f.read(reinterpret_cast<char*>(&ll), 1);
        const uint16_t seg_len = static_cast<uint16_t>((lh << 8) | ll);

        // SOF 标记：C0-C3, C5-C7, C9-CB, CD-CF
        const bool is_sof =
            (mk >= 0xC0 && mk <= 0xC3) ||
            (mk >= 0xC5 && mk <= 0xC7) ||
            (mk >= 0xC9 && mk <= 0xCB) ||
            (mk >= 0xCD && mk <= 0xCF);

        if (is_sof) {
            uint8_t prec, hh, hl, wh, wl;
            f.read(reinterpret_cast<char*>(&prec), 1);
            f.read(reinterpret_cast<char*>(&hh),   1);
            f.read(reinterpret_cast<char*>(&hl),   1);
            f.read(reinterpret_cast<char*>(&wh),   1);
            f.read(reinterpret_cast<char*>(&wl),   1);
            h = static_cast<uint32_t>((hh << 8) | hl);
            w = static_cast<uint32_t>((wh << 8) | wl);
            return true;
        }

        const int64_t skip = static_cast<int64_t>(seg_len) - 2;
        if (skip > 0) f.seekg(skip, std::ios::cur);
    }
    return false;
}

// ================================================================
// 文件 I/O 工具
// ================================================================

// 获取文件大小（字节）
static uint64_t file_size(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("Cannot stat: " + path);
    return static_cast<uint64_t>(f.tellg());
}

// 流式写入（1 MB 缓冲块，避免将整个 JPEG 一次性加载到内存）
static void stream_file(std::ofstream& dst, const std::string& path) {
    std::ifstream src(path, std::ios::binary);
    if (!src) throw std::runtime_error("Cannot open for reading: " + path);

    constexpr size_t CHUNK = 1u << 20; // 1 MB
    std::vector<char> buf(CHUNK);

    while (src) {
        src.read(buf.data(), static_cast<std::streamsize>(CHUNK));
        const auto n = src.gcount();
        if (n > 0) dst.write(buf.data(), n);
    }

    if (dst.fail())
        throw std::runtime_error("Write error while streaming: " + path);
}

// ================================================================
// write_csp —— 主入口
// ================================================================
void write_csp(const std::string& output_path, const CspWriteConfig& config) {

    // ============================================================
    // Step 1：预扫描文件，计算像素数据区的偏移量
    // ============================================================

    // 关联图像按 image_type 升序排列（label=0, macro=1, thumbnail=2）。
    // 像素区布局：tile 数据在前，assoc 图像在后（与参考文件一致）。
    // 这样所有 assoc 图像的 data_offset 均为正数，都能被 parser 的 if(data_offset) 加载。
    std::vector<AssocImageInput> sorted_assoc = config.associated_images;
    std::sort(sorted_assoc.begin(), sorted_assoc.end(),
              [](const AssocImageInput& a, const AssocImageInput& b) {
                  return a.image_type < b.image_type; // 升序：label(0), macro(1), thumbnail(2)
              });

    // 内部布局记录
    struct AssocRec {
        uint8_t type;
        uint32_t w, h;
        uint64_t offset, length;
        std::string path;
    };
    struct TileRec {
        uint32_t w, h, x, y;
        uint64_t offset;
        uint32_t length;
        std::string path;
    };
    struct FrameRec {
        uint32_t id, fw, fh;
        float ratio;
        std::vector<TileRec> tiles;
    };

    // pix_cursor 从 0 开始，记录相对于 pixel_file_offset 的偏移。
    // openslide 读取位置 = file.pixel_file_offset + tile_info->data_offset。
    // 测试验证：绝对偏移会导致 openslide 双重计数（pixel_file_offset+absolute_pos），
    // CSP 格式规范要求存储相对偏移（从 0 起）。
    uint64_t pix_cursor = 0;

    // --- 金字塔各层 tile 先分配偏移（tile 数据在像素区前部）---
    std::vector<FrameRec> frame_recs;
    for (const auto& fi : config.frames) {
        FrameRec fr;
        fr.id    = fi.frame_id;
        fr.fw    = fi.frame_width;
        fr.fh    = fi.frame_height;
        fr.ratio = fi.frame_ratio;

        for (const auto& ti : fi.tiles) {
            TileRec tr;
            tr.x = ti.pixel_x;
            tr.y = ti.pixel_y;
            // width/height 为 0 时从 JPEG SOF 读取实际尺寸，确保边缘 tile 尺寸正确
            if (ti.width != 0 && ti.height != 0) {
                tr.w = ti.width;
                tr.h = ti.height;
            } else {
                if (!jpeg_dims(ti.jpeg_path, tr.w, tr.h))
                    throw std::runtime_error(
                        "Cannot read tile JPEG dimensions: " + ti.jpeg_path);
            }
            tr.path   = ti.jpeg_path;
            tr.offset = pix_cursor; // 相对于 pixel_file_offset 的偏移
            tr.length = static_cast<uint32_t>(file_size(ti.jpeg_path));
            pix_cursor += tr.length;
            fr.tiles.push_back(std::move(tr));
        }
        frame_recs.push_back(std::move(fr));
    }

    // --- 关联图像在 tile 数据之后分配偏移（offset > 0，全部可被 parser 加载）---
    std::vector<AssocRec> assoc_recs;
    for (const auto& ai : sorted_assoc) {
        AssocRec r;
        r.type   = ai.image_type;
        r.path   = ai.jpeg_path;
        r.offset = pix_cursor; // tile 数据之后，必然非零
        r.length = file_size(ai.jpeg_path);
        if (!jpeg_dims(ai.jpeg_path, r.w, r.h))
            throw std::runtime_error("Cannot read JPEG dimensions: " + ai.jpeg_path);
        pix_cursor += r.length;
        assoc_recs.push_back(std::move(r));
    }

    const uint64_t total_pixel_bytes = pix_cursor;

    // ============================================================
    // Step 2：在内存中构建元数据字节树
    // ============================================================

    // ---- UID_EQUIPMENT ----
    Buf equip_ch;
    leaf(equip_ch, UID_MANUFACTURER, V_raw(config.manufacturer));
    leaf(equip_ch, UID_MODEL_NAME,   V_raw(config.model_name));
    Buf entry_equipment;
    seq(entry_equipment, UID_EQUIPMENT, 2u, equip_ch);

    // ---- UID_ASSOCIATED_IMAGE × N（data_offset 相对于 pixel_file_offset）----
    Buf entries_assoc;
    for (const auto& r : assoc_recs) {
        Buf ch;
        leaf(ch, UID_IMAGE_TYPE,       V_u8 (r.type));
        leaf(ch, UID_IMAGE_WIDTH,      V_u32(r.w));
        leaf(ch, UID_IMAGE_HEIGHT,     V_u32(r.h));
        leaf(ch, UID_IMAGE_DATA_OFFSET,V_u64(r.offset));
        leaf(ch, UID_IMAGE_DATA_LENGTH,V_u64(r.length));
        seq(entries_assoc, UID_ASSOCIATED_IMAGE, 5u, ch);
    }

    // ---- UID_SCAN_CONFIGURATION ----
    Buf scan_cfg_ch;
    leaf(scan_cfg_ch, UID_SCAN_TIME,          V_str(config.scan_time));
    leaf(scan_cfg_ch, UID_SLICE_BASIC_WIDTH,  V_u32(config.tile_width));
    leaf(scan_cfg_ch, UID_SLICE_BASIC_HEIGHT, V_u32(config.tile_height));
    leaf(scan_cfg_ch, UID_SCAN_RATIO,         V_f32(config.scan_ratio));
    leaf(scan_cfg_ch, UID_SCAN_MPP,           V_f32(config.mpp));
    leaf(scan_cfg_ch, UID_DOWNSAMPLING_RATIO, V_f32(config.downsampling_ratio));
    Buf entry_scan_cfg;
    seq(entry_scan_cfg, UID_SCAN_CONFIGURATION, 6u, scan_cfg_ch);

    // ---- 金字塔层级树 ----
    // UID_MULTI_FRAME_INFO
    //   └─ UID_FRAME_INFO × N（每层一个）
    //       ├─ UID_FRMAE_ID / UID_FRMAE_RATIO / UID_FRAME_WIDTH / UID_FRAME_HEIGHT
    //       └─ UID_MULTI_TILE_INFO
    //           └─ UID_TILE_INFO × M（每层所有 tile）
    Buf multi_frame_ch;
    uint32_t nframes = 0;

    for (const auto& fr : frame_recs) {
        // 构建 UID_MULTI_TILE_INFO（子节点：所有 UID_TILE_INFO）
        Buf tile_ch;
        for (const auto& tr : fr.tiles) {
            leaf(tile_ch, UID_TILE_INFO,
                 make_tile_blob(tr.w, tr.h, tr.offset, tr.length, tr.x, tr.y));
        }
        Buf entry_multi_tile;
        seq(entry_multi_tile, UID_MULTI_TILE_INFO,
            static_cast<uint32_t>(fr.tiles.size()), tile_ch);

        // 构建 UID_FRAME_INFO（5 个子节点）
        Buf frame_ch;
        leaf(frame_ch, UID_FRMAE_ID,     V_u32(fr.id));
        leaf(frame_ch, UID_FRAME_WIDTH,  V_u32(fr.fw));
        leaf(frame_ch, UID_FRAME_HEIGHT, V_u32(fr.fh));
        leaf(frame_ch, UID_FRMAE_RATIO,  V_f32(fr.ratio));
        cat (frame_ch, entry_multi_tile);
        seq(multi_frame_ch, UID_FRAME_INFO, 5u, frame_ch);
        ++nframes;
    }

    Buf entry_multi_frame;
    seq(entry_multi_frame, UID_MULTI_FRAME_INFO, nframes, multi_frame_ch);

    // UID_IMAGE_INFO（2 个子节点：image_id + multi_frame_info）
    Buf image_info_ch;
    leaf(image_info_ch, UID_IMAGE_ID, V_u32(0u));
    cat (image_info_ch, entry_multi_frame);
    Buf entry_image_info;
    seq(entry_image_info, UID_IMAGE_INFO, 2u, image_info_ch);

    // UID_MULTI_IMAGE_INFO（1 个子节点：image_info）
    Buf entry_multi_image;
    seq(entry_multi_image, UID_MULTI_IMAGE_INFO, 1u, entry_image_info);

    // UID_SCAN_RESULT（2 个子节点：scan_config + multi_image_info）
    Buf scan_result_ch;
    cat(scan_result_ch, entry_scan_cfg);
    cat(scan_result_ch, entry_multi_image);
    Buf entry_scan_result;
    seq(entry_scan_result, UID_SCAN_RESULT, 2u, scan_result_ch);

    // UID_MULTI_SCAN_RESULT（1 个子节点：scan_result）
    Buf entry_multi_scan;
    seq(entry_multi_scan, UID_MULTI_SCAN_RESULT, 1u, entry_scan_result);

    // UID_PIXEL_DATA 条目头
    // 解析器遇到此 UID 时仅记录 pixel_file_offset，不读取 value 内容；
    // value_length 填像素区总字节数，供解析器计算文件末尾并终止循环。
    Buf entry_pixel_hdr;
    emit_hdr(entry_pixel_hdr, UID_PIXEL_DATA, DT_LEAF,
             0u, total_pixel_bytes);

    // ============================================================
    // Step 3：写入文件
    // 文件结构（与真实扫描仪输出一致）：
    //   [0..127]        : 128 字节文件头（含 "MEDIC" 魔术字节）
    //   [128~]          : UID_EQUIPMENT
    //                     UID_ASSOCIATED_IMAGE × N
    //                     UID_PIXEL_DATA 条目头（22 字节）
    //                     [像素数据区：JPEG 文件]   ← PIXEL_DATA 的 value 内容
    //                     UID_MULTI_SCAN_RESULT      ← 在像素数据之后！
    // ============================================================
    std::ofstream out(output_path, std::ios::binary);
    if (!out)
        throw std::runtime_error("Cannot create output file: " + output_path);

    // 128 字节文件头
    std::array<uint8_t, 128> file_hdr{};
    // Magic "MEDIC"（真实扫描仪文件必须有此魔术字节）
    file_hdr[0] = 'M'; file_hdr[1] = 'E'; file_hdr[2] = 'D';
    file_hdr[3] = 'I'; file_hdr[4] = 'C';
    // offset_type = 64（U64 寻址）
    file_hdr[12] = 64u;
    file_hdr[13] = 0u;
    // 字节 30-37（规范第 31-38 字节）：multi scan result offset，
    // 即 UID_MULTI_SCAN_RESULT 条目在文件中的绝对偏移（小端 uint64）。
    // 之前这里硬编码了参考文件的值 0x00FEB0A0，与本文件的实际位置不符。
    // Bytes 30-37: absolute file offset of the UID_MULTI_SCAN_RESULT entry
    // (spec bytes 31-38). Previously hard-coded from a reference file.
    const uint64_t multi_scan_result_offset =
        128u + entry_equipment.size() + entries_assoc.size() +
        entry_pixel_hdr.size() + total_pixel_bytes;
    for (int i = 0; i < 8; ++i)
        file_hdr[30 + i] = static_cast<uint8_t>(multi_scan_result_offset >> (8 * i));
    // 字节 40：file type，1 = 2.0 版本单层明场
    file_hdr[40] = 0x01u;
    out.write(reinterpret_cast<const char*>(file_hdr.data()), 128);

    // 设备信息 + 关联图像
    out.write(reinterpret_cast<const char*>(entry_equipment.data()),
              static_cast<std::streamsize>(entry_equipment.size()));
    out.write(reinterpret_cast<const char*>(entries_assoc.data()),
              static_cast<std::streamsize>(entries_assoc.size()));

    // UID_PIXEL_DATA 条目头（22 字节，U64 模式）
    // pixel_file_offset = 当前文件位置（头部 + equipment + assoc + 22 字节 PIXEL_DATA 头之后）
    out.write(reinterpret_cast<const char*>(entry_pixel_hdr.data()),
              static_cast<std::streamsize>(entry_pixel_hdr.size()));

    if (!out.good())
        throw std::runtime_error("I/O error writing header section to: " + output_path);

    // 像素数据区：金字塔 tile 先写（与参考文件布局一致）
    uint64_t total_tiles = 0;
    for (size_t fi = 0; fi < frame_recs.size(); ++fi) {
        const auto& fr = frame_recs[fi];
        std::cout << "  [frame " << fi << "] "
                  << fr.fw << "×" << fr.fh
                  << "  ratio=" << fr.ratio << "x"
                  << "  tiles=" << fr.tiles.size() << "\n";
        for (const auto& tr : fr.tiles)
            stream_file(out, tr.path);
        total_tiles += fr.tiles.size();
    }

    // 像素数据区：关联图像在 tile 数据之后（offset 均 > 0）
    for (const auto& r : assoc_recs) {
        const char* type_name[] = {"label", "macro", "thumbnail"};
        std::cout << "  [assoc] " << type_name[r.type]
                  << "  " << r.w << "×" << r.h
                  << "  offset=" << r.offset
                  << "  (" << r.length << " bytes)\n";
        stream_file(out, r.path);
    }

    if (!out.good())
        throw std::runtime_error("I/O error writing pixel data to: " + output_path);

    // UID_MULTI_SCAN_RESULT 写在像素数据之后（与真实扫描仪格式一致）
    out.write(reinterpret_cast<const char*>(entry_multi_scan.data()),
              static_cast<std::streamsize>(entry_multi_scan.size()));

    if (!out.good())
        throw std::runtime_error("I/O error writing MULTI_SCAN_RESULT to: " + output_path);

    out.close();
    std::cout << "\nDone.\n"
              << "  Output  : " << output_path << "\n"
              << "  Frames  : " << frame_recs.size() << "\n"
              << "  Tiles   : " << total_tiles << "\n"
              << "  Assoc   : " << assoc_recs.size() << "\n"
              << "  Pix data: " << total_pixel_bytes << " bytes\n";
}

} // namespace csp
