#include "xgen_parse.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <numeric>
#include <string>
#include <tuple>
#include <vector>

#include <zlib.h>

#include "error.h"
#include "json.h"
#include "xgcore/xgcore.h"

namespace xgcore {
namespace {

// The Python reference caps block scanning at 100 blocks per buffer.
const size_t kMaxBlocks = 100;

struct Block {
  size_t start;  // payload start (block header + 16)
  size_t end;    // payload end (exclusive)
  uint32_t type;
};

// Mirrors the Python get_blocks(): each block starts with u32 type_code and
// u64 size at fixed offsets, followed by a 16-byte header and the payload.
std::vector<Block> get_blocks(const uint8_t* data, size_t len) {
  std::vector<Block> blocks;
  size_t address = 0;
  while (address + 16 <= len && blocks.size() < kMaxBlocks) {
    uint32_t type_code = static_cast<uint32_t>(data[address]) |
                         (static_cast<uint32_t>(data[address + 1]) << 8) |
                         (static_cast<uint32_t>(data[address + 2]) << 16) |
                         (static_cast<uint32_t>(data[address + 3]) << 24);
    uint64_t size = 0;
    for (int i = 0; i < 8; ++i) {
      size |= static_cast<uint64_t>(data[address + 8 + i]) << (8 * i);
    }
    const size_t payload_start = address + 16;
    const size_t payload_end = payload_start + static_cast<size_t>(size);
    if (payload_end > len) break;  // malformed tail; caller reports
    blocks.push_back({payload_start, payload_end, type_code});
    address = payload_end;
  }
  return blocks;
}

uint32_t read_u32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t read_u64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
  return v;
}

float read_f32(const uint8_t* p) {
  uint32_t bits = read_u32(p);
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

int malformed(const std::string& message) {
  set_error(message);
  return XG_ERR_MALFORMED;
}

struct GroupData {
  std::vector<uint8_t> bytes;
  std::vector<Block> inner;
  bool loaded = false;
};

// Loads (and optionally inflates) one group and builds its inner block table.
// Group index g refers to outer block g+1 (block 0 is the JSON header).
int load_group(const std::vector<Block>& outer, const uint8_t* raw,
               size_t raw_len, int group, bool deflated, GroupData& out) {
  if (out.loaded) return XG_OK;
  if (group < 0 || group + 1 >= static_cast<int>(outer.size())) {
    return malformed("group index out of range");
  }
  const Block& b = outer[static_cast<size_t>(group) + 1];
  if (deflated) {
    if (b.start + 32 > raw_len) {
      return malformed("deflated group header truncated");
    }
    z_stream stream{};
    if (inflateInit(&stream) != Z_OK) {
      return malformed("inflateInit failed");
    }
    stream.next_in =
        const_cast<Bytef*>(reinterpret_cast<const Bytef*>(raw + b.start + 32));
    stream.avail_in = static_cast<uInt>(raw_len - (b.start + 32));
    std::vector<uint8_t> chunk(1u << 20);
    int ret = Z_OK;
    do {
      stream.next_out = chunk.data();
      stream.avail_out = static_cast<uInt>(chunk.size());
      ret = inflate(&stream, Z_NO_FLUSH);
      if (ret != Z_OK && ret != Z_STREAM_END) break;
      out.bytes.insert(out.bytes.end(), chunk.data(),
                       chunk.data() + chunk.size() - stream.avail_out);
    } while (ret == Z_OK);
    inflateEnd(&stream);
    if (ret != Z_STREAM_END) {
      return malformed("zlib inflate failed for group");
    }
  } else {
    out.bytes.assign(raw + b.start, raw + b.end);
  }
  out.inner = get_blocks(out.bytes.data(), out.bytes.size());
  if (out.inner.empty()) {
    return malformed("no inner blocks in group");
  }
  out.loaded = true;
  return XG_OK;
}

}  // namespace

int parse_frame(const uint8_t* raw, size_t raw_len, bool sort_by_uv,
                ParsedFrame& out) {
  out = ParsedFrame();
  if (raw == nullptr || raw_len == 0) {
    return malformed("empty input");
  }

  const std::vector<Block> outer = get_blocks(raw, raw_len);
  if (outer.size() < 2) {
    return malformed("expected a header block and at least one group block");
  }

  // JSON header (block 0).
  const Block& header_block = outer[0];
  std::string header_text(
      reinterpret_cast<const char*>(raw + header_block.start),
      header_block.end - header_block.start);
  Json root;
  try {
    root = Json::parse(header_text);
  } catch (const JsonError& e) {
    return malformed(std::string("header JSON parse error: ") + e.what());
  }
  const Json* header = root.find("Header");
  if (header == nullptr || !header->is_object()) {
    return malformed("header JSON missing Header object");
  }
  const Json* group_count_json = header->find("GroupCount");
  if (group_count_json == nullptr || !group_count_json->is_int()) {
    return malformed("header JSON missing GroupCount");
  }
  const int64_t group_count = group_count_json->as_int();
  if (group_count < 0 || group_count + 1 > static_cast<int64_t>(outer.size())) {
    return malformed("GroupCount does not match the block table");
  }
  bool deflated = true;
  if (const Json* value = header->find("GroupDeflate")) {
    if (!value->is_bool()) return malformed("GroupDeflate must be a boolean");
    deflated = value->as_bool();
  }
  if (const Json* value = header->find("GroupBase64")) {
    if (value->is_bool() && value->as_bool()) {
      set_error("GroupBase64=true is not supported");
      return XG_ERR_UNSUPPORTED;
    }
  }

  // Item references: key -> ordered list of (group, index) pairs, exactly as
  // the Python readItems collects them across Items[] and RefMeshArray[].
  std::map<std::string, std::vector<std::pair<int, int>>> items;
  auto collect = [&items](const Json* array) {
    if (array == nullptr || !array->is_array()) return;
    for (const Json& entry : array->as_array()) {
      if (!entry.is_object()) continue;
      for (const auto& kv : entry.as_object()) {
        if (!kv.second.is_int()) continue;
        const uint64_t packed = static_cast<uint64_t>(kv.second.as_int());
        const int group = static_cast<int>(packed >> 32);
        const int index = static_cast<int>(packed & 0xFFFFFFFFu);
        items[kv.first].emplace_back(group, index);
      }
    }
  };
  collect(root.find("Items"));
  collect(root.find("RefMeshArray"));

  std::vector<GroupData> groups(static_cast<size_t>(group_count));
  std::vector<std::vector<uint8_t>> prim_payloads, pos_payloads,
      width_payloads, face_id_payloads, face_uv_payloads;

  auto decode_payloads =
      [&](const std::string& key,
          std::vector<std::vector<uint8_t>>& payloads) -> int {
    const auto it = items.find(key);
    if (it == items.end()) return XG_OK;
    payloads.reserve(it->second.size());
    for (const auto& gi : it->second) {
      int rc =
          load_group(outer, raw, raw_len, gi.first, deflated, groups[gi.first]);
      if (rc != XG_OK) return rc;
      const GroupData& group = groups[gi.first];
      if (gi.second < 0 || gi.second >= static_cast<int>(group.inner.size())) {
        return malformed("item index out of range");
      }
      const Block& inner = group.inner[static_cast<size_t>(gi.second)];
      payloads.emplace_back(group.bytes.begin() + inner.start,
                            group.bytes.begin() + inner.end);
    }
    return XG_OK;
  };

  int rc = decode_payloads("PrimitiveInfos", prim_payloads);
  if (rc != XG_OK) return rc;
  rc = decode_payloads("Positions", pos_payloads);
  if (rc != XG_OK) return rc;
  rc = decode_payloads("WIDTH_CV", width_payloads);
  if (rc != XG_OK) return rc;
  rc = decode_payloads("FaceId", face_id_payloads);
  if (rc != XG_OK) return rc;
  rc = decode_payloads("FaceUV", face_uv_payloads);
  if (rc != XG_OK) return rc;
  if (prim_payloads.size() != pos_payloads.size() ||
      prim_payloads.size() != width_payloads.size()) {
    return malformed("PrimitiveInfos/Positions/WIDTH_CV segment counts differ");
  }

  // Build strands. Records with length < 2 are skipped exactly like the
  // Python exporter; the remaining CVs are concatenated in raw order.
  int64_t cv_cursor = 0;
  for (size_t j = 0; j < prim_payloads.size(); ++j) {
    const auto& prim = prim_payloads[j];
    const auto& pos = pos_payloads[j];
    const auto& widths = width_payloads[j];
    const size_t record_count = prim.size() / 12;
    for (size_t r = 0; r < record_count; ++r) {
      const uint8_t* record = prim.data() + r * 12;
      const uint32_t offset = read_u32(record);
      const uint64_t length = read_u64(record + 4);
      ++out.total_records;
      if (length < 2) continue;
      const size_t pos_byte = static_cast<size_t>(offset) * 12;
      const size_t pos_bytes = static_cast<size_t>(length) * 12;
      if (pos_byte + pos_bytes > pos.size()) {
        return malformed("strand position range exceeds the Positions block");
      }
      const size_t width_byte = static_cast<size_t>(offset) * 4;
      const size_t width_bytes = static_cast<size_t>(length) * 4;
      if (width_byte + width_bytes > widths.size()) {
        return malformed("strand width range exceeds the WIDTH_CV block");
      }
      out.strands.push_back({static_cast<uint32_t>(cv_cursor), length});
      const size_t old_size = out.positions.size();
      out.positions.resize(old_size + static_cast<size_t>(length) * 3);
      std::memcpy(out.positions.data() + old_size, pos.data() + pos_byte,
                  pos_bytes);
      const size_t old_widths = out.widths.size();
      out.widths.resize(old_widths + static_cast<size_t>(length));
      std::memcpy(out.widths.data() + old_widths, widths.data() + width_byte,
                  width_bytes);
      cv_cursor += static_cast<int64_t>(length);
    }
  }
  out.num_curves = static_cast<int64_t>(out.strands.size());
  out.num_cvs = cv_cursor;

  // FaceId / FaceUV, when present, cover every strand record (including
  // skipped length<2 ones). They are decoded in raw order regardless of the
  // sort flag so callers can drive guide-id-from-ptex.
  if (!face_id_payloads.empty() || !face_uv_payloads.empty()) {
    if (face_id_payloads.size() != prim_payloads.size() ||
        face_uv_payloads.size() != prim_payloads.size()) {
      return malformed("FaceId/FaceUV segment counts differ");
    }
    for (size_t j = 0; j < face_id_payloads.size(); ++j) {
      const auto& ids = face_id_payloads[j];
      const auto& uvs = face_uv_payloads[j];
      if (uvs.size() != ids.size() * 2) {
        return malformed("FaceId/FaceUV size mismatch within a segment");
      }
      const size_t n = ids.size() / 4;
      for (size_t r = 0; r < n; ++r) {
        out.face_ids.push_back(
            static_cast<int32_t>(read_u32(ids.data() + r * 4)));
        out.face_uvs.push_back(read_f32(uvs.data() + r * 8));
        out.face_uvs.push_back(read_f32(uvs.data() + r * 8 + 4));
      }
    }
    if (static_cast<int64_t>(out.face_ids.size()) != out.total_records) {
      return malformed("FaceId count does not match PrimitiveInfos");
    }
  }

  sort_frame(out, sort_by_uv);
  return XG_OK;
}

}  // namespace xgcore
