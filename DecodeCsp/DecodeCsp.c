#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

// 独立 CSP 解析器（不依赖 OpenSlide / glib）。
// 功能目标：
// 1. 解析 CSP 元数据树（UID/DT/value 结构）。
// 2. 解析金字塔层与瓦片索引。
// 3. 按 layer + 瓦片位置提取原始 JPEG 瓦片字节。
// 4. 收集“UID -> 数值列表”的数字字典，便于业务侧直接读取。
namespace csp {

// CSP 条目头中 value_num / value_length 的编码宽度类型。
constexpr uint16_t kOffsetTypeU16 = 16;
constexpr uint16_t kOffsetTypeU32 = 32;
constexpr uint16_t kOffsetTypeU64 = 64;

// 设备信息相关 UID。
constexpr uint32_t UID_EQUIPMENT = 0x00010001u;
constexpr uint32_t UID_MANUFACTURER = 0x00010002u;
constexpr uint32_t UID_MODEL_NAME = 0x00010003u;

// 图像与金字塔信息相关 UID。
constexpr uint32_t UID_ASSOCIATED_IMAGE = 0x00020001u;
constexpr uint32_t UID_IMAGE_TYPE = 0x00020002u;
constexpr uint32_t UID_IMAGE_WIDTH = 0x00020003u;
constexpr uint32_t UID_IMAGE_HEIGHT = 0x00020004u;
constexpr uint32_t UID_IMAGE_DATA_OFFSET = 0x00020005u;
constexpr uint32_t UID_IMAGE_DATA_LENGTH = 0x00020006u;
constexpr uint32_t UID_MULTI_IMAGE_INFO = 0x00020009u;
constexpr uint32_t UID_IMAGE_INFO = 0x0002000Au;
constexpr uint32_t UID_IMAGE_ID = 0x0002000Bu;
constexpr uint32_t UID_MULTI_FRAME_INFO = 0x0002001Eu;
constexpr uint32_t UID_FRAME_INFO = 0x0002001Fu;
constexpr uint32_t UID_FRAME_ID = 0x00020020u;
constexpr uint32_t UID_FRAME_RATIO = 0x00020021u;
constexpr uint32_t UID_FRAME_WIDTH = 0x00020022u;
constexpr uint32_t UID_FRAME_HEIGHT = 0x00020023u;
constexpr uint32_t UID_MULTI_TILE_INFO = 0x00020024u;
constexpr uint32_t UID_TILE_INFO = 0x00020025u;

// 像素数据入口 UID：其 value 是大块二进制图像数据。
constexpr uint32_t UID_PIXEL_DATA = 0x00030001u;

// 扫描配置相关 UID。
constexpr uint32_t UID_SCAN_CONFIGURATION = 0x00040001u;
constexpr uint32_t UID_SCAN_TIME = 0x00040003u;
constexpr uint32_t UID_SLICE_BASIC_WIDTH = 0x00040007u;
constexpr uint32_t UID_SLICE_BASIC_HEIGHT = 0x00040008u;
constexpr uint32_t UID_SCAN_RATIO = 0x00040009u;
constexpr uint32_t UID_SCAN_MPP = 0x0004000Au;

// 扫描结果入口 UID。
constexpr uint32_t UID_MULTI_SCAN_RESULT = 0x00050001u;
constexpr uint32_t UID_SCAN_RESULT = 0x00050002u;

// 降采样比例 UID。
constexpr uint32_t UID_DOWNSAMPLING_RATIO = 0x00060002u;

// DT=0x000e 表示 Sequence（即“子条目数组”）。
constexpr uint16_t DT_SEQUENCE = 0x000eu;

// 伴随图像类型定义。
enum class AssociatedImageType : uint8_t {
  kLabel = 0,      // 标签图
  kPreview = 1,    // 预览图 / 宏图
  kThumbnail = 2,  // 缩略图
};

// 伴随图像索引信息（相对 UID_PIXEL_DATA 起始位置）。
struct CspAssociatedImageInfo {
  AssociatedImageType image_type = AssociatedImageType::kLabel;  // 图像类型
  uint32_t width = 0;                                             // 图像宽
  uint32_t height = 0;                                            // 图像高
  uint64_t data_offset = 0;                                       // 相对像素段偏移
  uint64_t data_length = 0;                                       // 数据长度
};

// 单个瓦片（通常是 JPEG）在文件中的索引信息。
struct CspTileInfo {
  uint32_t x = 0;            // 瓦片左上角 X（像素坐标，层内）
  uint32_t y = 0;            // 瓦片左上角 Y（像素坐标，层内）
  uint32_t width = 0;        // 瓦片宽
  uint32_t height = 0;       // 瓦片高
  uint64_t data_offset = 0;  // 瓦片数据相对 pixel_data 起始偏移
  uint64_t data_length = 0;  // 瓦片字节长度
  uint32_t crc32 = 0;        // 瓦片 CRC32（若文件中提供）
};

// 一层金字塔（frame）信息。
struct CspFrameInfo {
  uint32_t frame_id = 0;                // 层 ID
  uint32_t frame_width = 0;             // 层宽
  uint32_t frame_height = 0;            // 层高
  float frame_ratio = 0.0f;             // 层比例字段（由文件给出）
  std::vector<CspTileInfo> tiles;       // 本层包含的全部瓦片
};

// 一张逻辑图像的信息（可能包含多个 frame）。
struct CspImageInfo {
  uint32_t image_id = 0;                // 图像 ID
  std::vector<CspFrameInfo> frames;     // 图像金字塔层列表
};

// 扫描配置（来自 scan configuration 节点）。
struct CspScanConfig {
  uint32_t tile_width = 0;          // 标准瓦片宽
  uint32_t tile_height = 0;         // 标准瓦片高
  uint32_t image_width = 0;         // 预留字段：整图宽（当前未从样例填充）
  uint32_t image_height = 0;        // 预留字段：整图高（当前未从样例填充）
  float scan_ratio = 0.0f;          // 物镜倍率（如 20/40）
  float mpp = 0.0f;                 // 每像素物理尺寸（microns per pixel）
  float downsampling_ratio = 0.0f;  // 层间降采样比例
  std::string scan_time;            // 扫描时间字符串
};

// 原始条目节点（解析时构建的中间树结构）。
struct DataEntry {
  uint32_t uid = 0;         // 条目 UID（由 mid/eid 组合）
  uint16_t dt = 0;          // 数据类型 DataType
  uint64_t value_num = 0;   // 元素数量（依 offset_type 解析）
  uint64_t value_length = 0;// value 总字节长度
  // 指向 CspReader::file_data_ 内存中的 value 起始位置，不额外拷贝。
  const uint8_t *value = nullptr;
  // 仅 dt == DT_SEQUENCE 时有效，表示子条目数组。
  std::vector<DataEntry> children;
};

// 小端 16 位读取辅助。
static uint16_t ReadLe16(const uint8_t *p) {
  return static_cast<uint16_t>(p[0]) |
         (static_cast<uint16_t>(p[1]) << 8);
}

// 小端 32 位读取辅助。
static uint32_t ReadLe32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) |
         (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

// 小端 64 位读取辅助。
static uint64_t ReadLe64(const uint8_t *p) {
  return static_cast<uint64_t>(p[0]) |
         (static_cast<uint64_t>(p[1]) << 8) |
         (static_cast<uint64_t>(p[2]) << 16) |
         (static_cast<uint64_t>(p[3]) << 24) |
         (static_cast<uint64_t>(p[4]) << 32) |
         (static_cast<uint64_t>(p[5]) << 40) |
         (static_cast<uint64_t>(p[6]) << 48) |
         (static_cast<uint64_t>(p[7]) << 56);
}

// 小端 4 字节按 IEEE754 解释为 float。
static float ReadLeFloat32(const uint8_t *p) {
  const uint32_t u = ReadLe32(p);
  float f = 0.0f;
  std::memcpy(&f, &u, sizeof(f));
  return f;
}

// 去掉字符串末尾 '\0'，避免显示时混入填充字符。
static std::string TrimNullTail(const std::string &s) {
  size_t end = s.size();
  while (end > 0 && s[end - 1] == '\0') {
    --end;
  }
  return s.substr(0, end);
}

// CSP 主读取器：
// - 负责文件加载、条目树构建、语义解析、瓦片提取与字典收集。
class CspReader {
 public:
  // 打开并解析 CSP 文件，构建可查询索引。
  // 输入：
  //   path  - 文件路径
  // 输出：
  //   error - 失败时返回错误文本（可选）
  // 返回：
  //   true 成功，false 失败
  bool Open(const std::string &path, std::string *error = nullptr) {
    Clear();
    path_ = path;

    if (!ReadWholeFile(path, &file_data_, error)) {
      return false;
    }

    if (file_data_.size() < 128) {
      SetError(error, "file is too small (< 128 bytes)");
      return false;
    }

    if (!(file_data_[0] == 'M' && file_data_[1] == 'E' &&
          file_data_[2] == 'D' && file_data_[3] == 'I' &&
          file_data_[4] == 'C')) {
      SetError(error, "invalid CSP signature: missing MEDIC");
      return false;
    }

    offset_type_ = ReadLe16(&file_data_[12]);
    if (offset_type_ != kOffsetTypeU16 &&
        offset_type_ != kOffsetTypeU32 &&
        offset_type_ != kOffsetTypeU64) {
      SetError(error, "unsupported offset type: " +
                           std::to_string(offset_type_));
      return false;
    }

    parsed_offset_ = 128;
    while (parsed_offset_ < file_data_.size()) {
      DataEntry entry;
      uint64_t next_offset = 0;
      if (!ReadTopLevelEntry(parsed_offset_, &entry, &next_offset, error)) {
        return false;
      }
      parsed_offset_ = next_offset;

      if (entry.uid != UID_PIXEL_DATA) {
        CollectNumericDictionary(entry);
        if (!ParseTopLevelEntry(entry, error)) {
          return false;
        }
      }
    }

    if (!images_.empty()) {
      primary_layers_ = images_[0].frames;
    }

    return true;
  }

  // 返回主图像金字塔层数。
  size_t LayerCount() const { return primary_layers_.size(); }

  // 返回主图像金字塔层列表引用。
  const std::vector<CspFrameInfo> &Layers() const { return primary_layers_; }

  // 返回扫描配置。
  const CspScanConfig &ScanConfig() const { return scan_config_; }

  // 返回设备厂商名。
  const std::string &Manufacturer() const { return manufacturer_; }

  // 返回设备型号名。
  const std::string &ModelName() const { return model_name_; }

  // 返回 UID_PIXEL_DATA 在文件中的起始绝对偏移。
  uint64_t PixelDataOffset() const { return pixel_file_offset_; }

  // 返回数字字典（UID -> 数值数组）。
  // 注意：所有数值已统一提升为 uint64_t，便于业务统一读取。
  const std::map<uint32_t, std::vector<uint64_t>> &GetNumericDictionary() const {
    return numeric_dict_;
  }

  // 按逻辑网格坐标读取瓦片：
  // layer: 层号
  // tile_col/tile_row: 网格列/行（基于 tile.x / tile.width 推导）
  // jpeg_bytes: 返回 JPEG 原始字节
  // error: 失败时错误文本
  bool ReadTileJpeg(uint32_t layer,
                    uint32_t tile_col,
                    uint32_t tile_row,
                    std::vector<uint8_t> *jpeg_bytes,
                    std::string *error = nullptr) const {
    if (jpeg_bytes == nullptr) {
      SetError(error, "output buffer is null");
      return false;
    }
    if (layer >= primary_layers_.size()) {
      SetError(error, "layer out of range");
      return false;
    }

    const CspFrameInfo &frame = primary_layers_[layer];
    for (const auto &tile : frame.tiles) {
      if (tile.width == 0 || tile.height == 0) {
        continue;
      }
      const uint32_t col = tile.x / tile.width;
      const uint32_t row = tile.y / tile.height;
      if (col == tile_col && row == tile_row) {
        return ReadTileRange(tile, jpeg_bytes, error);
      }
    }

    SetError(error, "tile not found in the specified layer");
    return false;
  }

  // 按瓦片左上角像素坐标读取瓦片（更接近底层索引定义）。
  bool ReadTileJpegByOrigin(uint32_t layer,
                            uint32_t tile_x,
                            uint32_t tile_y,
                            std::vector<uint8_t> *jpeg_bytes,
                            std::string *error = nullptr) const {
    if (jpeg_bytes == nullptr) {
      SetError(error, "output buffer is null");
      return false;
    }
    if (layer >= primary_layers_.size()) {
      SetError(error, "layer out of range");
      return false;
    }

    const CspFrameInfo &frame = primary_layers_[layer];
    for (const auto &tile : frame.tiles) {
      if (tile.x == tile_x && tile.y == tile_y) {
        return ReadTileRange(tile, jpeg_bytes, error);
      }
    }

    SetError(error, "tile origin not found in the specified layer");
    return false;
  }

 private:
  // 统一写错误文本，避免重复判空逻辑。
  static void SetError(std::string *error, const std::string &msg) {
    if (error != nullptr) {
      *error = msg;
    }
  }

  // 一次性读取整个文件到内存。
  // 这里采用“全量读入 + 指针引用”的模式，后续递归解析无需频繁 I/O。
  static bool ReadWholeFile(const std::string &path,
                            std::vector<uint8_t> *out,
                            std::string *error) {
    std::ifstream fin(path, std::ios::binary);
    if (!fin) {
      SetError(error, "failed to open file: " + path);
      return false;
    }

    fin.seekg(0, std::ios::end);
    const std::streamoff size = fin.tellg();
    if (size < 0) {
      SetError(error, "failed to get file size");
      return false;
    }
    fin.seekg(0, std::ios::beg);

    out->assign(static_cast<size_t>(size), 0);
    if (size > 0) {
      fin.read(reinterpret_cast<char *>(out->data()), size);
      if (!fin) {
        SetError(error, "failed to read file bytes");
        return false;
      }
    }

    return true;
  }

  // 计算单个 DataEntry 头部长度。
  // 固定部分：mid(2) + eid(2) + dt(2) = 6 字节
  // 可变部分：value_num + value_length（由 offset_type 决定宽度）
  size_t HeaderSize() const {
    size_t n = 6;
    if (offset_type_ == kOffsetTypeU16) {
      n += 2 + 2;
    } else if (offset_type_ == kOffsetTypeU32) {
      n += 4 + 4;
    } else {
      n += 8 + 8;
    }
    return n;
  }

  // 从缓冲区反序列化 DataEntry 头部字段，不解析 value 本体。
  // consumed 返回实际消耗的头部字节数，供上层推进偏移。
  bool DeserializeHeader(const uint8_t *buf,
                         size_t remaining,
                         DataEntry *entry,
                         size_t *consumed,
                         std::string *error) const {
    const size_t need = HeaderSize();
    if (remaining < need) {
      SetError(error, "not enough bytes for entry header");
      return false;
    }

    size_t idx = 0;
    const uint16_t mid = ReadLe16(buf + idx);
    idx += 2;
    const uint16_t eid = ReadLe16(buf + idx);
    idx += 2;
    entry->uid = (static_cast<uint32_t>(mid) << 16) | eid;

    entry->dt = ReadLe16(buf + idx);
    idx += 2;

    if (offset_type_ == kOffsetTypeU16) {
      entry->value_num = ReadLe16(buf + idx);
      idx += 2;
      entry->value_length = ReadLe16(buf + idx);
      idx += 2;
    } else if (offset_type_ == kOffsetTypeU32) {
      entry->value_num = ReadLe32(buf + idx);
      idx += 4;
      entry->value_length = ReadLe32(buf + idx);
      idx += 4;
    } else {
      entry->value_num = ReadLe64(buf + idx);
      idx += 8;
      entry->value_length = ReadLe64(buf + idx);
      idx += 8;
    }

    *consumed = idx;
    return true;
  }

  // 递归构建 Sequence 子节点树。
  // 规则：当 dt == DT_SEQUENCE 时，value 内部由 value_num 个子条目顺序拼接而成。
  bool BuildChildren(DataEntry *entry, std::string *error) const {
    if (entry->dt != DT_SEQUENCE) {
      return true;
    }

    if (entry->value == nullptr && entry->value_length > 0) {
      SetError(error, "sequence entry has null value pointer");
      return false;
    }

    uint64_t idx = 0;
    entry->children.clear();
    entry->children.reserve(static_cast<size_t>(entry->value_num));

    for (uint64_t i = 0; i < entry->value_num; ++i) {
      DataEntry child;
      size_t consumed = 0;
      if (!DeserializeHeader(entry->value + idx,
                             static_cast<size_t>(entry->value_length - idx),
                             &child,
                             &consumed,
                             error)) {
        return false;
      }

      idx += static_cast<uint64_t>(consumed);
      if (idx > entry->value_length ||
          child.value_length > (entry->value_length - idx)) {
        SetError(error, "child value length out of range");
        return false;
      }

      child.value = entry->value + idx;
      if (!BuildChildren(&child, error)) {
        return false;
      }

      idx += child.value_length;
      entry->children.push_back(std::move(child));
    }

    if (idx != entry->value_length) {
      SetError(error, "sequence value has leftover bytes");
      return false;
    }

    return true;
  }

  // 读取一个顶层条目，并按需递归解析其子节点。
  // next_offset 返回“下一个顶层条目”的起始偏移。
  bool ReadTopLevelEntry(uint64_t offset,
                         DataEntry *entry,
                         uint64_t *next_offset,
                         std::string *error) {
    const size_t hdr = HeaderSize();
    if (offset > file_data_.size() ||
        hdr > file_data_.size() - static_cast<size_t>(offset)) {
      SetError(error, "top-level header out of file range");
      return false;
    }

    size_t consumed = 0;
    if (!DeserializeHeader(file_data_.data() + offset,
                           file_data_.size() - static_cast<size_t>(offset),
                           entry,
                           &consumed,
                           error)) {
      return false;
    }

    const uint64_t value_offset = offset + consumed;
    if (value_offset > file_data_.size() ||
        entry->value_length > (file_data_.size() - static_cast<size_t>(value_offset))) {
      SetError(error, "top-level value out of file range");
      return false;
    }

    entry->value = file_data_.data() + value_offset;
    *next_offset = value_offset + entry->value_length;

    // UID_PIXEL_DATA 的 value 是整个像素区大块数据：
    // 仅记录其绝对起始偏移，避免把像素数据当元数据递归解析。
    if (entry->uid == UID_PIXEL_DATA) {
      pixel_file_offset_ = value_offset;
      return true;
    }

    return BuildChildren(entry, error);
  }

  // 顶层语义分发：仅处理当前关心的业务节点，其余节点忽略。
  bool ParseTopLevelEntry(const DataEntry &entry, std::string *error) {
    switch (entry.uid) {
      case UID_EQUIPMENT:
        return ParseEquipment(entry, error);
      case UID_ASSOCIATED_IMAGE:
        return ParseAssociatedImage(entry, error);
      case UID_MULTI_SCAN_RESULT:
        return ParseMultiScanResult(entry, error);
      default:
        return true;
    }
  }

  // 解析设备信息（厂商、型号）。
  bool ParseEquipment(const DataEntry &entry, std::string *error) {
    if (entry.dt != DT_SEQUENCE) {
      SetError(error, "equipment entry is not a sequence");
      return false;
    }

    for (const auto &child : entry.children) {
      if (child.uid == UID_MANUFACTURER) {
        manufacturer_ = TrimNullTail(std::string(
            reinterpret_cast<const char *>(child.value),
            static_cast<size_t>(child.value_length)));
      } else if (child.uid == UID_MODEL_NAME) {
        model_name_ = TrimNullTail(std::string(
            reinterpret_cast<const char *>(child.value),
            static_cast<size_t>(child.value_length)));
      }
    }

    return true;
  }

  // 解析伴随图索引信息（label/preview/thumbnail）。
  bool ParseAssociatedImage(const DataEntry &entry, std::string *error) {
    if (entry.dt != DT_SEQUENCE) {
      SetError(error, "associated image entry is not a sequence");
      return false;
    }

    CspAssociatedImageInfo image;
    bool has_type = false;

    for (const auto &child : entry.children) {
      switch (child.uid) {
        case UID_IMAGE_TYPE: {
          if (child.value_length < 1) {
            SetError(error, "associated image type value is empty");
            return false;
          }
          const uint8_t t = child.value[0];
          if (t > 2) {
            SetError(error, "associated image type is out of range");
            return false;
          }
          image.image_type = static_cast<AssociatedImageType>(t);
          has_type = true;
          break;
        }
        case UID_IMAGE_WIDTH:
          if (child.value_length >= 4) {
            image.width = ReadLe32(child.value);
          }
          break;
        case UID_IMAGE_HEIGHT:
          if (child.value_length >= 4) {
            image.height = ReadLe32(child.value);
          }
          break;
        case UID_IMAGE_DATA_OFFSET:
          if (child.value_length >= 8) {
            image.data_offset = ReadLe64(child.value);
          }
          break;
        case UID_IMAGE_DATA_LENGTH:
          if (child.value_length >= 8) {
            image.data_length = ReadLe64(child.value);
          }
          break;
        default:
          break;
      }
    }

    if (has_type) {
      associated_images_[static_cast<size_t>(image.image_type)] = image;
    }

    return true;
  }

  // 解析单个瓦片记录。
  // 布局固定为：
  // width(u32), height(u32), data_offset(u64), data_length(u64),
  // x(u32), y(u32), crc32(u32)
  bool ParseTileInfo(const DataEntry &entry, CspTileInfo *tile, std::string *error) {
    if (tile == nullptr) {
      SetError(error, "tile target is null");
      return false;
    }
    if (entry.value_length < (4 + 4 + 8 + 8 + 4 + 4 + 4)) {
      SetError(error, "tile info value is too short");
      return false;
    }

    size_t idx = 0;
    tile->width = ReadLe32(entry.value + idx);
    idx += 4;
    tile->height = ReadLe32(entry.value + idx);
    idx += 4;
    tile->data_offset = ReadLe64(entry.value + idx);
    idx += 8;
    tile->data_length = ReadLe64(entry.value + idx);
    idx += 8;
    tile->x = ReadLe32(entry.value + idx);
    idx += 4;
    tile->y = ReadLe32(entry.value + idx);
    idx += 4;
    tile->crc32 = ReadLe32(entry.value + idx);

    return true;
  }

  // 解析某一层里的多瓦片列表。
  bool ParseMultiTiles(const DataEntry &entry, CspFrameInfo *frame, std::string *error) {
    if (frame == nullptr) {
      SetError(error, "frame target is null");
      return false;
    }

    frame->tiles.clear();
    frame->tiles.reserve(entry.children.size());

    for (const auto &child : entry.children) {
      if (child.uid != UID_TILE_INFO) {
        continue;
      }

      CspTileInfo tile;
      if (!ParseTileInfo(child, &tile, error)) {
        return false;
      }
      frame->tiles.push_back(tile);
    }

    return true;
  }

  // 解析单层 frame 的信息。
  bool ParseFrameInfo(const DataEntry &entry, CspFrameInfo *frame, std::string *error) {
    if (frame == nullptr) {
      SetError(error, "frame target is null");
      return false;
    }

    for (const auto &child : entry.children) {
      switch (child.uid) {
        case UID_FRAME_ID:
          if (child.value_length >= 4) {
            frame->frame_id = ReadLe32(child.value);
          }
          break;
        case UID_FRAME_RATIO:
          if (child.value_length >= 4) {
            frame->frame_ratio = ReadLeFloat32(child.value);
          }
          break;
        case UID_FRAME_WIDTH:
          if (child.value_length >= 4) {
            frame->frame_width = ReadLe32(child.value);
          }
          break;
        case UID_FRAME_HEIGHT:
          if (child.value_length >= 4) {
            frame->frame_height = ReadLe32(child.value);
          }
          break;
        case UID_MULTI_TILE_INFO:
          if (!ParseMultiTiles(child, frame, error)) {
            return false;
          }
          break;
        default:
          break;
      }
    }

    return true;
  }

  // 解析多层 frame 列表（即金字塔层列表）。
  bool ParseMultiFrameInfo(const DataEntry &entry,
                           std::vector<CspFrameInfo> *frames,
                           std::string *error) {
    if (frames == nullptr) {
      SetError(error, "frames target is null");
      return false;
    }

    frames->clear();
    for (const auto &child : entry.children) {
      if (child.uid != UID_FRAME_INFO) {
        continue;
      }

      CspFrameInfo frame;
      if (!ParseFrameInfo(child, &frame, error)) {
        return false;
      }
      frames->push_back(std::move(frame));
    }

    return true;
  }

  // 解析单个 image 节点。
  bool ParseImageInfo(const DataEntry &entry, CspImageInfo *image, std::string *error) {
    if (image == nullptr) {
      SetError(error, "image target is null");
      return false;
    }

    for (const auto &child : entry.children) {
      switch (child.uid) {
        case UID_IMAGE_ID:
          if (child.value_length >= 4) {
            image->image_id = ReadLe32(child.value);
          }
          break;
        case UID_MULTI_FRAME_INFO:
          if (!ParseMultiFrameInfo(child, &image->frames, error)) {
            return false;
          }
          break;
        default:
          break;
      }
    }

    return true;
  }

  // 解析扫描配置中的单个字段。
  void ParseScanConfigPart(const DataEntry &entry) {
    switch (entry.uid) {
      case UID_SCAN_TIME:
        scan_config_.scan_time = TrimNullTail(std::string(
            reinterpret_cast<const char *>(entry.value),
            static_cast<size_t>(entry.value_length)));
        break;
      case UID_SLICE_BASIC_WIDTH:
        if (entry.value_length >= 4) {
          scan_config_.tile_width = ReadLe32(entry.value);
        }
        break;
      case UID_SLICE_BASIC_HEIGHT:
        if (entry.value_length >= 4) {
          scan_config_.tile_height = ReadLe32(entry.value);
        }
        break;
      case UID_SCAN_RATIO:
        if (entry.value_length >= 4) {
          scan_config_.scan_ratio = ReadLeFloat32(entry.value);
        }
        break;
      case UID_SCAN_MPP:
        if (entry.value_length >= 4) {
          scan_config_.mpp = ReadLeFloat32(entry.value);
        }
        break;
      case UID_DOWNSAMPLING_RATIO:
        if (entry.value_length >= 4) {
          scan_config_.downsampling_ratio = ReadLeFloat32(entry.value);
        }
        break;
      default:
        break;
    }
  }

  // 解析扫描配置集合，并做最基本合法性校验。
  bool ParseScanConfig(const DataEntry &entry, std::string *error) {
    for (const auto &child : entry.children) {
      ParseScanConfigPart(child);
    }

    if (scan_config_.tile_width == 0 || scan_config_.tile_height == 0) {
      SetError(error, "invalid scan config: tile size is zero");
      return false;
    }

    return true;
  }

  // 解析多图像信息。
  // 当前实现会保留全部 image，但对外默认使用 images_[0] 作为主图像。
  bool ParseMultiImageInfo(const DataEntry &entry, std::string *error) {
    images_.clear();

    for (const auto &child : entry.children) {
      if (child.uid != UID_IMAGE_INFO) {
        continue;
      }
      CspImageInfo image;
      if (!ParseImageInfo(child, &image, error)) {
        return false;
      }
      images_.push_back(std::move(image));
    }

    if (images_.empty()) {
      SetError(error, "no image info found in scan result");
      return false;
    }

    return true;
  }

  // 解析 scan_result 节点（配置 + 图像层级索引）。
  bool ParseScanResult(const DataEntry &entry, std::string *error) {
    for (const auto &child : entry.children) {
      if (child.uid == UID_SCAN_CONFIGURATION) {
        if (!ParseScanConfig(child, error)) {
          return false;
        }
      } else if (child.uid == UID_MULTI_IMAGE_INFO) {
        if (!ParseMultiImageInfo(child, error)) {
          return false;
        }
      }
    }
    return true;
  }

  // 解析 multi_scan_result 节点（当前常见为单个 scan_result）。
  bool ParseMultiScanResult(const DataEntry &entry, std::string *error) {
    for (const auto &child : entry.children) {
      if (child.uid == UID_SCAN_RESULT) {
        if (!ParseScanResult(child, error)) {
          return false;
        }
      }
    }
    return true;
  }

  // 数值叶子节点收集策略（启发式）：
  // 仅当 value 可被 value_num 整除，且元素宽度为 1/2/4/8 字节时，
  // 才将其按“同构数值数组”解释并写入字典。
  void CollectNumericLeaf(const DataEntry &entry) {
    if (entry.value == nullptr || entry.value_length == 0 || entry.value_num == 0) {
      return;
    }

    if (entry.value_length % entry.value_num != 0) {
      return;
    }

    const uint64_t element_size = entry.value_length / entry.value_num;
    if (element_size != 1 && element_size != 2 && element_size != 4 && element_size != 8) {
      return;
    }

    auto &bucket = numeric_dict_[entry.uid];
    for (uint64_t i = 0; i < entry.value_num; ++i) {
      const uint8_t *p = entry.value + (i * element_size);
      uint64_t v = 0;
      if (element_size == 1) {
        v = p[0];
      } else if (element_size == 2) {
        v = ReadLe16(p);
      } else if (element_size == 4) {
        v = ReadLe32(p);
      } else {
        v = ReadLe64(p);
      }
      bucket.push_back(v);
    }
  }

  // 深度遍历元数据树，收集全部可识别的数值叶子节点。
  void CollectNumericDictionary(const DataEntry &entry) {
    if (entry.dt != DT_SEQUENCE) {
      CollectNumericLeaf(entry);
      return;
    }

    for (const auto &child : entry.children) {
      CollectNumericDictionary(child);
    }
  }

  // 根据瓦片索引从 file_data_ 提取原始字节：
  // 瓦片绝对起始偏移 = pixel_file_offset_ + tile.data_offset
  bool ReadTileRange(const CspTileInfo &tile,
                     std::vector<uint8_t> *jpeg_bytes,
                     std::string *error) const {
    const uint64_t begin = pixel_file_offset_ + tile.data_offset;
    const uint64_t end = begin + tile.data_length;

    if (begin > file_data_.size() || end > file_data_.size() || begin > end) {
      SetError(error, "tile range is outside file bounds");
      return false;
    }

    jpeg_bytes->assign(file_data_.begin() + static_cast<size_t>(begin),
                       file_data_.begin() + static_cast<size_t>(end));
    return true;
  }

  // 重置对象状态，确保 Open 可重复调用且不带脏数据。
  void Clear() {
    path_.clear();
    file_data_.clear();
    offset_type_ = kOffsetTypeU64;
    parsed_offset_ = 0;
    pixel_file_offset_ = 0;
    manufacturer_.clear();
    model_name_.clear();
    associated_images_ = {};
    scan_config_ = {};
    images_.clear();
    primary_layers_.clear();
    numeric_dict_.clear();
  }

 private:
  std::string path_;   // 当前打开的文件路径（用于定位/调试）
  // CSP 全文件缓冲区（解析过程中的所有 value 指针都引用此内存）。
  std::vector<uint8_t> file_data_;
  uint16_t offset_type_ = kOffsetTypeU64;  // 条目头编码宽度类型（16/32/64）
  uint64_t parsed_offset_ = 0;             // 顶层解析游标（绝对偏移）
  uint64_t pixel_file_offset_ = 0;         // UID_PIXEL_DATA 的 value 起始绝对偏移

  std::string manufacturer_;  // 设备厂商
  std::string model_name_;    // 设备型号
  // 三类伴随图索引表，下标与 AssociatedImageType 一一对应。
  std::array<CspAssociatedImageInfo, 3> associated_images_{};
  CspScanConfig scan_config_{};  // 扫描配置缓存

  std::vector<CspImageInfo> images_;  // 文件内全部 image 节点
  // 对外默认读取的主金字塔层集合（当前策略：images_[0].frames）。
  std::vector<CspFrameInfo> primary_layers_;

  // 数字字典：UID -> 展平后的数值数组（统一为 uint64_t）。
  std::map<uint32_t, std::vector<uint64_t>> numeric_dict_;
};

// 将字节数组完整写入二进制文件。
static bool WriteFile(const std::string &path, const std::vector<uint8_t> &data) {
  std::ofstream fout(path, std::ios::binary);
  if (!fout) {
    return false;
  }
  if (!data.empty()) {
    fout.write(reinterpret_cast<const char *>(data.data()),
               static_cast<std::streamsize>(data.size()));
  }
  return static_cast<bool>(fout);
}

// 安全解析无符号 32 位整数参数（用于命令行层号/坐标输入）。
static bool ParseUint32(const char *s, uint32_t *v) {
  if (s == nullptr || v == nullptr) {
    return false;
  }
  std::stringstream ss(s);
  unsigned long long x = 0;
  ss >> x;
  if (!ss || !ss.eof() || x > 0xFFFFFFFFull) {
    return false;
  }
  *v = static_cast<uint32_t>(x);
  return true;
}

}  // namespace csp

// 定义 CSP_DECODE_NO_MAIN 后可把本文件作为库源码包含（例如 tests/），不编译命令行入口。
// Define CSP_DECODE_NO_MAIN to include this file as a library (e.g. from tests/).
#ifndef CSP_DECODE_NO_MAIN
int main(int argc, char **argv) {
  if (argc < 2) {
    std::cerr << "Usage:\n"
              << "  DecodeCsp <csp_path>\n"
              << "  DecodeCsp <csp_path> <layer> <tile_col> <tile_row> [out.jpg]\n";
    return 1;
  }

  // 轻量命令行流程：
  // 1) 先解析并打印基础信息；
  // 2) 若提供 layer/col/row，则额外导出对应瓦片 JPEG。
  csp::CspReader reader;
  std::string error;
  if (!reader.Open(argv[1], &error)) {
    std::cerr << "Open failed: " << error << "\n";
    return 2;
  }

  // 打印解析后的关键元数据，便于快速核对解析正确性。
  std::cout << "Open OK\n";
  std::cout << "  Manufacturer: " << reader.Manufacturer() << "\n";
  std::cout << "  Model: " << reader.ModelName() << "\n";
  std::cout << "  ScanTime: " << reader.ScanConfig().scan_time << "\n";
  std::cout << "  TileSize: " << reader.ScanConfig().tile_width
            << "x" << reader.ScanConfig().tile_height << "\n";
  std::cout << "  LayerCount: " << reader.LayerCount() << "\n";
  std::cout << "  PixelDataOffset: " << reader.PixelDataOffset() << "\n";

  const auto &layers = reader.Layers();
  // 输出每一层的尺寸和瓦片数量。
  for (size_t i = 0; i < layers.size(); ++i) {
    std::cout << "  Layer[" << i << "]: "
              << layers[i].frame_width << "x" << layers[i].frame_height
              << ", tiles=" << layers[i].tiles.size() << "\n";
  }

  const auto &dict = reader.GetNumericDictionary();
  // 仅展示前 20 个 UID，避免输出过长。
  std::cout << "  NumericDictionary keys: " << dict.size() << "\n";
  size_t shown = 0;
  for (const auto &kv : dict) {
    if (shown >= 20) {
      std::cout << "  ... (more keys omitted)\n";
      break;
    }
    std::cout << "    UID 0x" << std::hex << kv.first << std::dec
              << " -> values=" << kv.second.size();
    if (!kv.second.empty()) {
      std::cout << ", first=" << kv.second[0];
    }
    std::cout << "\n";
    ++shown;
  }

  if (argc >= 5) {
    // 解析 layer/tile_col/tile_row 参数。
    uint32_t layer = 0;
    uint32_t col = 0;
    uint32_t row = 0;
    if (!csp::ParseUint32(argv[2], &layer) ||
        !csp::ParseUint32(argv[3], &col) ||
        !csp::ParseUint32(argv[4], &row)) {
      std::cerr << "Invalid layer/tile_col/tile_row arguments\n";
      return 3;
    }

    std::vector<uint8_t> jpeg;
    // 从指定层与网格位置读取原始 JPEG 瓦片字节。
    if (!reader.ReadTileJpeg(layer, col, row, &jpeg, &error)) {
      std::cerr << "ReadTileJpeg failed: " << error << "\n";
      return 4;
    }

    std::string out = (argc >= 6)
                          ? argv[5]
                          : (std::string("tile_L") + argv[2] + "_C" + argv[3] +
                             "_R" + argv[4] + ".jpg");
    // 导出瓦片到磁盘，文件名可由第 6 个参数覆盖。
    if (!csp::WriteFile(out, jpeg)) {
      std::cerr << "Failed to write tile file: " << out << "\n";
      return 5;
    }

    std::cout << "Tile saved: " << out << " (" << jpeg.size() << " bytes)\n";
  }

  return 0;
}
#endif  // CSP_DECODE_NO_MAIN
