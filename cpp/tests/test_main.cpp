#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <zlib.h>

#include <Alembic/Abc/All.h>
#include <Alembic/AbcCoreOgawa/All.h>
#include <Alembic/AbcGeom/All.h>

#include "json.h"
#include "xgen_memory_internal.h"
#include "xgen_parse.h"
#include "xgcore/xgcore.h"

namespace {

int g_failures = 0;

namespace Abc = Alembic::Abc;
namespace AbcGeom = Alembic::AbcGeom;
namespace AbcA = Alembic::AbcCoreAbstract;
namespace AbcCoreOgawa = Alembic::AbcCoreOgawa;

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

void test_parse_face() {
  SyntheticFrame frame = make_synthetic_frame(false);
  XgParseInfo info{};
  int rc = xg_parse_frame_size(frame.raw.data(), frame.raw.size(), 1, &info);
  CHECK(rc == XG_OK);
  CHECK(info.total_records == 3);

  const int64_t n = info.total_records;
  std::vector<int32_t> face_ids(static_cast<size_t>(n));
  std::vector<float> face_uvs(static_cast<size_t>(2 * n));
  std::vector<int32_t> order(static_cast<size_t>(n));
  int64_t count = 0;
  rc = xg_parse_face(frame.raw.data(), frame.raw.size(), 1, face_ids.data(),
                     face_uvs.data(), order.data(), &count);
  CHECK(rc == XG_OK);
  CHECK(count == 3);
  // Raw order face ids 2, 0, 1.
  CHECK(face_ids[0] == 2 && face_ids[1] == 0 && face_ids[2] == 1);
  // Raw order face uvs: (0.5,0.1), (0.1,0.9), (0.5,0.1).
  CHECK(face_uvs[0] == 0.5f && face_uvs[1] == 0.1f);
  CHECK(face_uvs[2] == 0.1f && face_uvs[3] == 0.9f);
  CHECK(face_uvs[4] == 0.5f && face_uvs[5] == 0.1f);
  // Sorted index order matches the frame order.
  CHECK(order[0] == 2 && order[1] == 0 && order[2] == 1);

  rc = xg_parse_face(frame.raw.data(), frame.raw.size(), 0, face_ids.data(),
                     face_uvs.data(), order.data(), &count);
  CHECK(rc == XG_OK);
  CHECK(order[0] == 0 && order[1] == 1 && order[2] == 2);

  rc = xg_parse_face(nullptr, 0, 0, nullptr, nullptr, nullptr, nullptr);
  CHECK(rc == XG_ERR_INVALID_ARG);
}

std::vector<float> cubic_knots(int64_t length) {
  std::vector<float> knots;
  const int64_t inside = length - 3 + 1;
  for (int k = 0; k < 3; ++k) knots.push_back(0.0f);
  for (int64_t k = 0; k < inside; ++k) knots.push_back(static_cast<float>(k));
  for (int k = 0; k < 3; ++k) {
    knots.push_back(static_cast<float>(inside - 1));
  }
  return knots;
}

// Verifies the archive at `path` exposes expected top-level object names and
// that each ICurves object has the expected sample/arb structure.
void check_multi_archive(const char* path,
                         const std::vector<std::string>& names,
                         const std::vector<std::vector<std::string>>& props,
                         size_t expected_samples) {
  try {
    Abc::IArchive archive(AbcCoreOgawa::ReadArchive(), path);
    Abc::IObject top = archive.getTop();
    CHECK(top.getNumChildren() == names.size());
    CHECK(props.size() == names.size());
    if (top.getNumChildren() != names.size() || props.size() != names.size()) {
      return;
    }
    for (size_t i = 0; i < names.size(); ++i) {
      Abc::IObject child = top.getChild(static_cast<AbcA::index_t>(i));
      CHECK(child.getName() == names[i]);
      if (!AbcGeom::ICurves::matches(child.getHeader())) {
        CHECK(false);
        continue;
      }
      AbcGeom::ICurves curves(child, Abc::kWrapExisting);
      AbcGeom::ICurvesSchema schema = curves.getSchema();
      if (schema.getNumSamples() != expected_samples) {
        CHECK(false);
        continue;
      }
      AbcGeom::ICurvesSchema::Sample sample;
      schema.get(sample, Abc::ISampleSelector(static_cast<AbcA::index_t>(0)));
      const Abc::Int32ArraySamplePtr nverts = sample.getCurvesNumVertices();
      const Abc::P3fArraySamplePtr positions = sample.getPositions();
      CHECK(nverts != nullptr && positions != nullptr);
      if (nverts == nullptr || positions == nullptr) continue;
      CHECK(nverts->size() == 2);
      CHECK((*nverts)[0] == 3 && (*nverts)[1] == 4);
      CHECK(positions->size() == 7);

      Abc::ICompoundProperty arb = schema.getArbGeomParams();
      std::vector<std::string> found_props;
      for (size_t p = 0; p < arb.getNumProperties(); ++p) {
        found_props.push_back(arb.getPropertyHeader(p).getName());
      }
      for (const std::string& name : props[i]) {
        bool found = false;
        for (const std::string& prop : found_props) {
          if (prop == name) found = true;
        }
        CHECK(found);
      }
    }
  } catch (const std::exception& e) {
    CHECK(false);
    std::printf("read-back failed: %s\n", e.what());
  }
}

void write_arrays_for_object(int object_handle, float offset) {
  const int32_t nverts[] = {3, 4};
  const int32_t orders[] = {4, 4};
  std::vector<float> positions(7 * 3);
  for (size_t i = 0; i < positions.size(); ++i) {
    positions[i] = offset + static_cast<float>(i);
  }
  std::vector<float> knots;
  std::vector<float> k0 = cubic_knots(3);
  std::vector<float> k1 = cubic_knots(4);
  knots.insert(knots.end(), k0.begin(), k0.end());
  knots.insert(knots.end(), k1.begin(), k1.end());
  const float widths[] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f};
  const float root_uv[] = {0.0f, 0.0f, 1.0f, 1.0f};

  CHECK(xg_abc_write_curves_first(object_handle, positions.data(), nverts,
                                  orders, knots.data(), widths, 2, 7,
                                  static_cast<int64_t>(knots.size())) == XG_OK);
  const char* names_a[] = {"alpha"};
  CHECK(xg_abc_write_arb_string(object_handle, "groom_group_name", names_a,
                                1) == XG_OK);
  const int32_t ids[] = {42};
  CHECK(xg_abc_write_arb_int32(object_handle, "groom_id", ids, 1) == XG_OK);
  const float weights[] = {1.0f, 1.0f};
  CHECK(xg_abc_write_arb_float(object_handle, "groom_weights", weights, 2) ==
        XG_OK);
  const float uvs[] = {0.0f, 0.5f, 1.0f, 0.5f};
  CHECK(xg_abc_write_arb_v2f(object_handle, "groom_uvs", uvs, 2) == XG_OK);
  CHECK(xg_abc_write_root_uv(object_handle, root_uv, 2) == XG_OK);

  std::vector<float> frame2(positions);
  for (size_t i = 0; i < frame2.size(); ++i) frame2[i] += 100.0f;
  CHECK(xg_abc_write_curves_frame(object_handle, frame2.data(), 7) == XG_OK);
}

void test_abc_multi_arrays() {
  const char* path = "xgcore_test_multi_arrays.abc";
  int archive_handle = 0;
  CHECK(xg_abc_open(path, 0.0, 0.0, 0, 0, &archive_handle) == XG_OK);
  int a = 0, b = 0;
  CHECK(xg_abc_add_curves(archive_handle, "obj_a", 1, -1, &a) == XG_OK);
  CHECK(xg_abc_add_curves(archive_handle, "obj_b", 0, 5, &b) == XG_OK);
  write_arrays_for_object(a, 0.0f);
  write_arrays_for_object(b, 1000.0f);
  CHECK(xg_abc_close(archive_handle) == XG_OK);

  std::vector<std::string> names = {"obj_a", "obj_b"};
  std::vector<std::vector<std::string>> props = {
      {"groom_group_name", "groom_guide", "groom_id", "groom_weights",
       "groom_uvs", "groom_root_uv"},
      {"groom_group_name", "groom_group_id", "groom_id", "groom_weights",
       "groom_uvs", "groom_root_uv"}};
  check_multi_archive(path, names, props, 2);

  // Error paths.
  CHECK(xg_abc_add_curves(9999, "x", 0, -1, &a) == XG_ERR_INVALID_ARG);
  int c = 0;
  CHECK(xg_abc_open("xgcore_test_state.abc", 0.0, 0.0, 0, 0, &archive_handle) ==
        XG_OK);
  CHECK(xg_abc_add_curves(archive_handle, "obj_c", 0, -1, &c) == XG_OK);
  const float pos[] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
  const int32_t nverts[] = {2};
  const int32_t orders[] = {2};
  const float knots[] = {0.0f, 0.0f, 1.0f, 1.0f};
  CHECK(xg_abc_write_curves_frame(c, pos, 2) == XG_ERR_STATE);
  CHECK(xg_abc_write_curves_first(c, pos, nverts, orders, knots, nullptr, 1, 2,
                                  4) == XG_OK);
  CHECK(xg_abc_write_curves_first(c, pos, nverts, orders, knots, nullptr, 1, 2,
                                  4) == XG_ERR_STATE);
  CHECK(xg_abc_close(archive_handle) == XG_OK);
  CHECK(xg_abc_close(archive_handle) == XG_ERR_INVALID_ARG);
}

void test_abc_multi_raw() {
  SyntheticFrame frame = make_synthetic_frame(true);
  const char* path = "xgcore_test_multi_raw.abc";
  int archive_handle = 0;
  CHECK(xg_abc_open(path, 0.0, 0.0, 0, 1, &archive_handle) == XG_OK);
  int a = 0, b = 0;
  CHECK(xg_abc_add_curves(archive_handle, "groom_a", 0, -1, &a) == XG_OK);
  CHECK(xg_abc_add_curves(archive_handle, "groom_b", 1, -1, &b) == XG_OK);
  CHECK(xg_abc_write_first(a, frame.raw.data(), frame.raw.size(), "groom_a", 0,
                           -1) == XG_OK);
  CHECK(xg_abc_write_first(b, frame.raw.data(), frame.raw.size(), "groom_b", 1,
                           -1) == XG_OK);
  CHECK(xg_abc_close(archive_handle) == XG_OK);
  try {
    Abc::IArchive archive(AbcCoreOgawa::ReadArchive(), path);
    Abc::IObject top = archive.getTop();
    CHECK(top.getNumChildren() == 2);
    if (top.getNumChildren() != 2) return;
    CHECK(top.getChild(0).getName() == "groom_a");
    CHECK(top.getChild(1).getName() == "groom_b");
    for (size_t i = 0; i < 2; ++i) {
      AbcGeom::ICurves curves(top.getChild(static_cast<AbcA::index_t>(i)),
                              Abc::kWrapExisting);
      AbcGeom::ICurvesSchema schema = curves.getSchema();
      CHECK(schema.getNumSamples() == 1);
      AbcGeom::ICurvesSchema::Sample sample;
      schema.get(sample, Abc::ISampleSelector(static_cast<AbcA::index_t>(0)));
      const Abc::Int32ArraySamplePtr nverts = sample.getCurvesNumVertices();
      const Abc::P3fArraySamplePtr positions = sample.getPositions();
      CHECK(nverts != nullptr && nverts->size() == 3);
      CHECK(positions != nullptr && positions->size() == 9);
    }
  } catch (const std::exception& e) {
    CHECK(false);
    std::printf("raw multi read-back failed: %s\n", e.what());
  }

  // Backward-compatible single-object flow: raw API on the archive handle
  // lazily creates one implicit object.
  const char* single_path = "xgcore_test_single.abc";
  CHECK(xg_abc_open(single_path, 0.0, 0.0, 0, 1, &archive_handle) == XG_OK);
  CHECK(xg_abc_write_first(archive_handle, frame.raw.data(), frame.raw.size(),
                           "implicit", 0, 3) == XG_OK);
  CHECK(xg_abc_close(archive_handle) == XG_OK);
  std::vector<std::string> single_names = {"implicit"};
  // The single-object path writes group name; other arb props differ, so only
  // check the object count and name here.
  try {
    Abc::IArchive archive(AbcCoreOgawa::ReadArchive(), single_path);
    CHECK(archive.getTop().getNumChildren() == 1);
    if (archive.getTop().getNumChildren() == 1) {
      CHECK(archive.getTop().getChild(0).getName() == "implicit");
    }
  } catch (const std::exception& e) {
    CHECK(false);
    std::printf("single read-back failed: %s\n", e.what());
  }
}

void test_abc_duplicate_group_name() {
  // Regression: the wrapper writes groom_group_name through the arb API
  // before xg_abc_write_first; the raw first-frame writer must not write it
  // again for explicit objects (previously raised "Already have a property
  // named: groom_group_name").
  SyntheticFrame frame = make_synthetic_frame(false);
  const char* path = "xgcore_test_dup.abc";
  int archive_handle = 0;
  CHECK(xg_abc_open(path, 0.0, 0.0, 0, 0, &archive_handle) == XG_OK);
  int obj = 0;
  CHECK(xg_abc_add_curves(archive_handle, "dup", 0, -1, &obj) == XG_OK);
  const char* name = "dup";
  CHECK(xg_abc_write_arb_string(obj, "groom_group_name", &name, 1) == XG_OK);
  CHECK(xg_abc_write_first(obj, frame.raw.data(), frame.raw.size(), "dup", 0,
                           -1) == XG_OK);
  CHECK(xg_abc_close(archive_handle) == XG_OK);
}

// Walks a real MSVC std::multimap with the same tree layout the memory
// reader uses for XgSplineData::items. The layout is compiler-specific, so
// this test only runs meaningfully on the MSVC build used for Maya plugins.
void test_memory_tree_walk() {
  std::multimap<std::string, std::shared_ptr<int>> m;
  m.emplace("b", std::make_shared<int>(2));
  m.emplace("a", std::make_shared<int>(1));
  m.emplace("c", std::make_shared<int>(3));
  const uint8_t* map_bytes = reinterpret_cast<const uint8_t*>(&m);
  const uint8_t* head =
      *reinterpret_cast<const uint8_t* const*>(map_bytes + 0x00);
  const uint64_t size = *reinterpret_cast<const uint64_t*>(map_bytes + 0x08);
  CHECK(size == 3);
  if (head == nullptr || size != 3) return;
  std::vector<int> values;
  const uint8_t* node = xgcore::mem::tree_leftmost(head);
  for (uint64_t i = 0; i < size; ++i) {
    if (node == head) return;
    values.push_back(*xgcore::mem::node_shared_value<int>(node));
    node = xgcore::mem::tree_next(node, head);
  }
  CHECK(values.size() == 3);
  if (values.size() == 3) {
    CHECK(values[0] == 1);
    CHECK(values[1] == 2);
    CHECK(values[2] == 3);
  }
  CHECK(node == head);
}

// sort_frame must match the Python exporter's ordering: lexicographic
// (faceId, u, v, index) over ALL records, with `order` covering the valid
// strands and `order_full` covering every record.
void test_sort_frame() {
  xgcore::ParsedFrame f;
  f.total_records = 4;
  f.num_curves = 3;
  f.strands = {{0, 2}, {2, 3}, {5, 4}};
  f.face_ids = {3, 1, 2, 1};
  f.face_uvs = {0.5f, 0.5f, 0.1f, 0.2f, 0.3f, 0.4f, 0.1f, 0.1f};

  xgcore::sort_frame(f, true);
  const std::vector<int32_t> expected_full{3, 1, 2, 0};
  const std::vector<int32_t> expected_order{3, 1, 2};
  CHECK(f.order_full == expected_full);
  CHECK(f.order == expected_order);

  xgcore::sort_frame(f, false);
  CHECK(f.order == std::vector<int32_t>({0, 1, 2}));
  CHECK(f.order_full == std::vector<int32_t>({0, 1, 2, 3}));
}

}  // namespace

int main() {
  test_json();
  test_parse_deflated();
  test_parse_raw();
  test_base64_unsupported();
  test_malformed();
  test_parse_face();
  test_abc_multi_arrays();
  test_abc_multi_raw();
  test_abc_duplicate_group_name();
  test_memory_tree_walk();
  test_sort_frame();

  if (g_failures == 0) {
    std::printf("xgcore_tests: all tests passed\n");
    return 0;
  }
  std::printf("xgcore_tests: %d failure(s)\n", g_failures);
  return 1;
}
