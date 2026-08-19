#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <zlib.h>

#include "json.h"
#include "xgcore/xgcore.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
      ++g_failures;                                                     \
    }                                                                   \
  } while (0)

void put_u32_le(std::vector<uint8_t>& out, size_t offset, uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    out[offset + i] = static_cast<uint8_t>(value >> (8 * i));
  }
}

void put_u32_le(uint8_t* p, size_t offset, uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    p[offset + i] = static_cast<uint8_t>(value >> (8 * i));
  }
}

void put_u64_le(std::vector<uint8_t>& out, size_t offset, uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    out[offset + i] = static_cast<uint8_t>(value >> (8 * i));
  }
}

void put_u64_le(uint8_t* p, size_t offset, uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    p[offset + i] = static_cast<uint8_t>(value >> (8 * i));
  }
}

// 16-byte block header + payload.
std::vector<uint8_t> make_block(const std::vector<uint8_t>& payload,
                                uint32_t type) {
  std::vector<uint8_t> out(16 + payload.size(), 0);
  put_u32_le(out, 0, type);
  put_u64_le(out, 8, payload.size());
  std::memcpy(out.data() + 16, payload.data(), payload.size());
  return out;
}

std::vector<uint8_t> zlib_compress(const std::vector<uint8_t>& data) {
  uLongf bound = compressBound(static_cast<uLong>(data.size()));
  std::vector<uint8_t> out(bound);
  uLongf out_len = bound;
  const int rc = compress2(out.data(), &out_len, data.data(),
                           static_cast<uLong>(data.size()), 9);
  CHECK(rc == Z_OK);
  out.resize(out_len);
  return out;
}

void put_f32(std::vector<uint8_t>& out, size_t offset, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, 4);
  put_u32_le(out, offset, bits);
}

struct SyntheticFrame {
  std::vector<uint8_t> raw;  // whole raw_data.bin equivalent
  std::vector<float> expected_positions;
  std::vector<float> expected_widths;
  std::vector<int32_t> expected_order;  // with sort
};

// Builds a one-group, deflated or raw XgSplineData buffer with 3 strands:
// lengths 3/4/2, face ids 2/0/1. Sorting by (faceId,u,v) yields strand
// order [1, 2, 0].
SyntheticFrame make_synthetic_frame(bool deflated) {
  SyntheticFrame frame;

  // Strand data (raw order): strand 0: CVs 0..2, strand 1: CVs 3..6,
  // strand 2: CVs 7..8.
  std::vector<uint32_t> offsets = {0, 3, 7};
  std::vector<uint64_t> lengths = {3, 4, 2};
  const int num_cvs = 9;
  std::vector<float> positions(static_cast<size_t>(num_cvs) * 3);
  for (int i = 0; i < num_cvs * 3; ++i) positions[i] = static_cast<float>(i);
  std::vector<float> widths(static_cast<size_t>(num_cvs));
  for (int i = 0; i < num_cvs; ++i) widths[i] = static_cast<float>(i) * 0.5f;

  std::vector<uint8_t> prim_payload;
  for (size_t i = 0; i < offsets.size(); ++i) {
    uint8_t record[12] = {};
    put_u32_le(record, 0, offsets[i]);
    put_u64_le(record, 4, lengths[i]);
    prim_payload.insert(prim_payload.end(), record, record + 12);
  }
  std::vector<uint8_t> pos_payload(positions.size() * 4);
  for (size_t i = 0; i < positions.size(); ++i) {
    put_f32(pos_payload, i * 4, positions[i]);
  }
  std::vector<uint8_t> width_payload(widths.size() * 4);
  for (size_t i = 0; i < widths.size(); ++i) {
    put_f32(width_payload, i * 4, widths[i]);
  }

  // Face ids/uvs (one entry per strand record). Face id order 2, 0, 1 sorts
  // to strand order [1, 2, 0].
  std::vector<int32_t> face_ids = {2, 0, 1};
  std::vector<float> face_uvs = {0.5f, 0.1f, 0.1f, 0.9f, 0.5f, 0.1f};
  std::vector<uint8_t> fid_payload(face_ids.size() * 4);
  for (size_t i = 0; i < face_ids.size(); ++i) {
    put_u32_le(fid_payload, i * 4, static_cast<uint32_t>(face_ids[i]));
  }
  std::vector<uint8_t> fuv_payload(face_uvs.size() * 4);
  for (size_t i = 0; i < face_uvs.size(); ++i) {
    put_f32(fuv_payload, i * 4, face_uvs[i]);
  }

  // Inner blocks in item index order: PrimitiveInfos=0, Positions=1,
  // WIDTH_CV=2, FaceId=3, FaceUV=4.
  std::vector<uint8_t> group_payload;
  const std::vector<std::vector<uint8_t>> inner_blocks = {
      make_block(prim_payload, 1), make_block(pos_payload, 1),
      make_block(width_payload, 1), make_block(fid_payload, 1),
      make_block(fuv_payload, 1)};
  for (const auto& block : inner_blocks) {
    group_payload.insert(group_payload.end(), block.begin(), block.end());
  }

  std::string header =
      std::string("{\"Header\":{\"Version\":1,\"Type\":\"XgSplineData\","
                  "\"GroupVersion\":1,\"GroupCount\":1,\"GroupBase64\":false,") +
      (deflated ? "\"GroupDeflate\":true,\"GroupDeflateLevel\":9"
                : "\"GroupDeflate\":false") +
      "},\"Items\":[{\"Name\":\"x\",\"Id\":\"{GUID}\",\"PrimitiveInfos\":0,"
      "\"Positions\":1,\"WIDTH_CV\":2,\"FaceId\":3,\"FaceUV\":4}],"
      "\"RefMeshArray\":[]}";
  std::vector<uint8_t> header_bytes(header.begin(), header.end());

  std::vector<uint8_t> outer_group;
  if (deflated) {
    std::vector<uint8_t> compressed = zlib_compress(group_payload);
    // Group block: 16-byte header + 32-byte sub-header + deflate stream.
    std::vector<uint8_t> body(32, 0);
    body.insert(body.end(), compressed.begin(), compressed.end());
    outer_group = make_block(body, 2);
  } else {
    outer_group = make_block(group_payload, 2);
  }

  std::vector<uint8_t> header_block = make_block(header_bytes, 1);
  frame.raw.insert(frame.raw.end(), header_block.begin(), header_block.end());
  frame.raw.insert(frame.raw.end(), outer_group.begin(), outer_group.end());
  frame.expected_positions = positions;
  frame.expected_widths = widths;
  // strand0 -> output 2, strand1 -> output 0, strand2 -> output 1.
  frame.expected_order = {2, 0, 1};
  return frame;
}

void test_json() {
  const std::string text =
      "{\"Header\":{\"Version\":1,\"GroupBase64\":false,\"Items\":[1,2,3],"
      "\"S\":\"a\\\"b\"},\"Flag\":true,\"Nothing\":null}";
  xgcore::Json root = xgcore::Json::parse(text);
  CHECK(root.is_object());
  const xgcore::Json* header = root.find("Header");
  CHECK(header != nullptr && header->is_object());
  CHECK(header->find("Version")->as_int() == 1);
  CHECK(header->find("GroupBase64")->as_bool() == false);
  CHECK(header->find("Items")->as_array().size() == 3);
  CHECK(header->find("S")->as_string() == "a\"b");
  CHECK(root.find("Flag")->as_bool() == true);
  CHECK(root.find("Nothing")->is_null());

  bool threw = false;
  try {
    xgcore::Json::parse("{\"bad\":1.5}");
  } catch (const xgcore::JsonError&) {
    threw = true;
  }
  CHECK(threw);
}

void check_parse(const SyntheticFrame& frame, bool sort) {
  XgParseInfo info{};
  int rc = xg_parse_frame_size(frame.raw.data(), frame.raw.size(), sort ? 1 : 0,
                               &info);
  CHECK(rc == XG_OK);
  if (rc != XG_OK) return;
  CHECK(info.num_curves == 3);
  CHECK(info.num_cvs == 9);

  std::vector<float> positions(static_cast<size_t>(info.num_cvs) * 3);
  std::vector<int32_t> curve_offsets(static_cast<size_t>(info.num_curves) + 1);
  std::vector<float> widths(static_cast<size_t>(info.num_cvs));
  std::vector<float> roots(static_cast<size_t>(info.num_curves) * 3);
  std::vector<int32_t> order(static_cast<size_t>(info.num_curves));
  rc = xg_parse_frame(frame.raw.data(), frame.raw.size(), sort ? 1 : 0,
                      positions.data(), curve_offsets.data(), widths.data(),
                      roots.data(), order.data(), &info);
  CHECK(rc == XG_OK);
  if (rc != XG_OK) return;

  if (sort) {
    // Expected sorted strand order is [1, 2, 0].
    const int sorted[] = {1, 2, 0};
    for (int i = 0; i < 3; ++i) {
      CHECK(order[i] == frame.expected_order[i]);
      const int len = sorted[i] == 1 ? 4 : (sorted[i] == 0 ? 3 : 2);
      CHECK(curve_offsets[i + 1] == curve_offsets[i] + len);
      const int base = (sorted[i] == 0 ? 0 : (sorted[i] == 1 ? 9 : 21));
      CHECK(roots[i * 3] == frame.expected_positions[base]);
    }
    // Sorted positions/widths: strand 1 (CV floats 9..20), strand 2
    // (21..26), strand 0 (0..8).
    float expected_sorted[27];
    float expected_widths[9];
    int idx = 0;
    int widx = 0;
    for (int s = 0; s < 3; ++s) {
      const int strand = sorted[s];
      const int start_cv = (strand == 0 ? 0 : (strand == 1 ? 3 : 7));
      const int len = (strand == 0 ? 3 : (strand == 1 ? 4 : 2));
      for (int k = 0; k < len * 3; ++k) {
        expected_sorted[idx++] = static_cast<float>(start_cv * 3 + k);
      }
      for (int k = 0; k < len; ++k) {
        expected_widths[widx++] = static_cast<float>(start_cv + k) * 0.5f;
      }
    }
    for (int i = 0; i < 27; ++i) CHECK(positions[i] == expected_sorted[i]);
    for (int i = 0; i < 9; ++i) CHECK(widths[i] == expected_widths[i]);
  } else {
    for (int i = 0; i < 3; ++i) CHECK(order[i] == i);
    CHECK(curve_offsets[0] == 0 && curve_offsets[3] == 9);
    CHECK(roots[0] == 0.0f && roots[3] == 9.0f && roots[6] == 21.0f);
    for (size_t i = 0; i < positions.size(); ++i) {
      CHECK(positions[i] == frame.expected_positions[i]);
    }
    for (size_t i = 0; i < widths.size(); ++i) {
      CHECK(widths[i] == frame.expected_widths[i]);
    }
  }
}

void test_parse_deflated() {
  SyntheticFrame frame = make_synthetic_frame(true);
  check_parse(frame, false);
  check_parse(frame, true);
}

void test_parse_raw() {
  SyntheticFrame frame = make_synthetic_frame(false);
  check_parse(frame, false);
  check_parse(frame, true);
}

void test_base64_unsupported() {
  SyntheticFrame frame = make_synthetic_frame(true);
  const std::string needle = "\"GroupBase64\":false";
  std::string text(reinterpret_cast<const char*>(frame.raw.data() + 16),
                   frame.raw.size() - 16);
  const size_t pos = text.find(needle);
  CHECK(pos != std::string::npos);
  // Keep the payload length identical so the block size stays valid.
  text.replace(pos, needle.size(), "\"GroupBase64\":true ");
  // Rebuild the full buffer: outer block header (first 16 bytes) + patched
  // header payload and the rest of the stream.
  std::vector<uint8_t> patched(frame.raw.begin(), frame.raw.begin() + 16);
  patched.insert(patched.end(), text.begin(), text.end());
  XgParseInfo info{};
  int rc = xg_parse_frame_size(patched.data(), patched.size(), 0, &info);
  CHECK(rc == XG_ERR_UNSUPPORTED);
}

void test_malformed() {
  const uint8_t garbage[] = {1, 2, 3, 4, 5};
  XgParseInfo info{};
  CHECK(xg_parse_frame_size(garbage, sizeof(garbage), 0, &info) ==
        XG_ERR_MALFORMED);
  CHECK(xg_parse_frame_size(nullptr, 0, 0, nullptr) == XG_ERR_INVALID_ARG);
}

}  // namespace

int main() {
  test_json();
  test_parse_deflated();
  test_parse_raw();
  test_base64_unsupported();
  test_malformed();

  if (g_failures == 0) {
    std::printf("xgcore_tests: all tests passed\n");
    return 0;
  }
  std::printf("xgcore_tests: %d failure(s)\n", g_failures);
  return 1;
}
