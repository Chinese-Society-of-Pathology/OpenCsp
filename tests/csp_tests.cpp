// OpenCsp smoke / round-trip tests.
//
// Usage:
//   csp_tests sample    <file.csp>
//       Opens the file with the reference decoder (DecodeCsp/DecodeCsp.c) and
//       checks that every tile of every layer can be read and is a complete
//       JPEG stream (FFD8 ... FFD9) lying inside its frame.
//   csp_tests roundtrip <encoded.csp> <slide_dir>
//       Checks a file produced by EncodeCsp/csp_writer from <slide_dir>:
//       every tile read back by the decoder must be byte-identical to the
//       source JPEG slide_dir/layer<frame_id>/<row>_<col>.jpeg, the tile counts
//       must match, and header bytes 30-37 (multi scan result offset) must point
//       at the UID_MULTI_SCAN_RESULT entry.
//
// Exit code 0 = pass, non-zero = failure.

#define CSP_DECODE_NO_MAIN
#include "../DecodeCsp/DecodeCsp.c"

#include <cstdio>
#include <fstream>
#include <iterator>

namespace {

int g_failures = 0;

void Fail(const std::string &msg) {
  std::cerr << "FAIL: " << msg << "\n";
  ++g_failures;
}

bool ReadFileBytes(const std::string &path, std::vector<uint8_t> *out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}

uint64_t Le64(const std::vector<uint8_t> &d, size_t off) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | d[off + static_cast<size_t>(i)];
  return v;
}

// Walks top-level entries (64-bit addressing only) and returns the absolute
// offset of the UID_MULTI_SCAN_RESULT entry header, or 0 if not found.
uint64_t FindMultiScanResultOffset(const std::vector<uint8_t> &d) {
  if (d.size() < 128 || d[12] != 64 || d[13] != 0) return 0;
  uint64_t off = 128;
  while (off + 22 <= d.size()) {
    const uint32_t uid = (static_cast<uint32_t>(d[off] | (d[off + 1] << 8)) << 16) |
                         static_cast<uint32_t>(d[off + 2] | (d[off + 3] << 8));
    if (uid == 0x00050001u) return off;
    const uint64_t vlen = Le64(d, static_cast<size_t>(off + 14));
    off += 22 + vlen;
  }
  return 0;
}

bool IsCompleteJpeg(const std::vector<uint8_t> &b) {
  return b.size() >= 4 && b[0] == 0xFF && b[1] == 0xD8 &&
         b[b.size() - 2] == 0xFF && b[b.size() - 1] == 0xD9;
}

int RunSample(const std::string &path) {
  csp::CspReader reader;
  std::string err;
  if (!reader.Open(path, &err)) {
    Fail("open " + path + ": " + err);
    return 1;
  }
  if (reader.LayerCount() == 0) Fail("no layers");
  size_t total = 0;
  for (size_t L = 0; L < reader.Layers().size(); ++L) {
    const auto &frame = reader.Layers()[L];
    if (frame.tiles.empty()) Fail("layer " + std::to_string(L) + " has no tiles");
    for (const auto &t : frame.tiles) {
      std::vector<uint8_t> jpeg;
      if (!reader.ReadTileJpegByOrigin(static_cast<uint32_t>(L), t.x, t.y, &jpeg, &err)) {
        Fail("layer " + std::to_string(L) + " tile (" + std::to_string(t.x) + "," +
             std::to_string(t.y) + "): " + err);
        continue;
      }
      if (!IsCompleteJpeg(jpeg))
        Fail("layer " + std::to_string(L) + " tile (" + std::to_string(t.x) + "," +
             std::to_string(t.y) + ") is not a complete JPEG");
      if (t.x >= frame.frame_width || t.y >= frame.frame_height)
        Fail("layer " + std::to_string(L) + " tile origin outside frame");
      ++total;
    }
  }

  // Informational only: report whether the header offset field matches.
  std::vector<uint8_t> raw;
  if (ReadFileBytes(path, &raw) && raw.size() >= 38) {
    const uint64_t header = Le64(raw, 30);
    const uint64_t actual = FindMultiScanResultOffset(raw);
    std::cout << "header multi_scan_result_offset=" << header << " actual=" << actual
              << (header == actual ? " (match)" : " (MISMATCH, informational)") << "\n";
  }

  std::cout << "sample: layers=" << reader.LayerCount() << " tiles=" << total
            << " failures=" << g_failures << "\n";
  return g_failures == 0 ? 0 : 1;
}

int RunRoundtrip(const std::string &csp_path, const std::string &slide_dir) {
  csp::CspReader reader;
  std::string err;
  if (!reader.Open(csp_path, &err)) {
    Fail("open " + csp_path + ": " + err);
    return 1;
  }
  const uint32_t tw = reader.ScanConfig().tile_width;
  const uint32_t th = reader.ScanConfig().tile_height;
  if (tw == 0 || th == 0) {
    Fail("tile size is zero");
    return 1;
  }

  size_t compared = 0;
  for (size_t L = 0; L < reader.Layers().size(); ++L) {
    const auto &frame = reader.Layers()[L];
    for (const auto &t : frame.tiles) {
      const std::string src = slide_dir + "/layer" + std::to_string(frame.frame_id) + "/" +
                              std::to_string(t.y / th) + "_" + std::to_string(t.x / tw) + ".jpeg";
      std::vector<uint8_t> expected, actual;
      if (!ReadFileBytes(src, &expected)) {
        Fail("missing source tile " + src);
        continue;
      }
      if (!reader.ReadTileJpegByOrigin(static_cast<uint32_t>(L), t.x, t.y, &actual, &err)) {
        Fail("read tile for " + src + ": " + err);
        continue;
      }
      if (actual != expected) Fail("tile bytes differ from " + src);
      ++compared;
    }
  }

  std::vector<uint8_t> raw;
  if (!ReadFileBytes(csp_path, &raw) || raw.size() < 128) {
    Fail("cannot re-read " + csp_path);
  } else {
    const uint64_t header = Le64(raw, 30);
    const uint64_t actual = FindMultiScanResultOffset(raw);
    if (actual == 0) Fail("UID_MULTI_SCAN_RESULT entry not found");
    if (header != actual)
      Fail("header multi_scan_result_offset=" + std::to_string(header) +
           " but entry is at " + std::to_string(actual));
  }

  std::cout << "roundtrip: layers=" << reader.LayerCount() << " tiles compared=" << compared
            << " failures=" << g_failures << "\n";
  if (compared == 0) Fail("no tiles compared");
  return g_failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char **argv) {
  if (argc == 3 && std::string(argv[1]) == "sample") return RunSample(argv[2]);
  if (argc == 4 && std::string(argv[1]) == "roundtrip") return RunRoundtrip(argv[2], argv[3]);
  std::cerr << "Usage:\n  csp_tests sample <file.csp>\n"
               "  csp_tests roundtrip <encoded.csp> <slide_dir>\n";
  return 2;
}
