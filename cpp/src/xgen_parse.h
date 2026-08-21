#ifndef XGCORE_XGEN_PARSE_H
#define XGCORE_XGEN_PARSE_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <vector>

namespace xgcore {

struct StrandInfo {
  uint32_t offset;  // first CV index into the positions float array
  uint64_t length;  // CV count
};

// A fully decoded frame. Strands/positions/widths/face data are kept in raw
// order; `order` maps an original strand index to its output index (identity
// when sorting is disabled).
struct ParsedFrame {
  int64_t total_records = 0;  // all PrimitiveInfos records (including length<2)
  int64_t num_curves = 0;
  int64_t num_cvs = 0;
  std::vector<StrandInfo> strands;
  std::vector<float> positions;  // num_cvs * 3, raw order
  std::vector<float> widths;     // num_cvs, raw order
  std::vector<int32_t> face_ids; // num_curves, raw order
  std::vector<float> face_uvs;   // num_curves * 2, raw order
  std::vector<int32_t> order;    // num_curves, raw index -> output index
  std::vector<int32_t> order_full; // all records, raw index -> sorted index
};

// Parses one frame of raw XGen spline data and optionally sorts strands by
// (faceId, u, v). Returns XG_OK or an XG_ERR_* code.
int parse_frame(const uint8_t* raw, size_t raw_len, bool sort_by_uv,
                ParsedFrame& out);

// Applies the (faceId, u, v) lexicographic ordering used by the Python
// exporter, or identity ordering when sort_by_uv is false. Shared by the
// serialized and direct-memory read paths. Inline so unit tests can exercise
// it without an extra DLL export.
inline void sort_frame(ParsedFrame& out, bool sort_by_uv) {
  if (sort_by_uv) {
    // Lexicographic (faceId, u, v, index) ordering identical to Python's
    // sorted((key, i) for i, key in enumerate(order_list)).
    struct Key {
      int32_t face_id;
      float u;
      float v;
      uint32_t index;
    };
    std::vector<Key> keys(static_cast<size_t>(out.total_records));
    for (size_t i = 0; i < keys.size(); ++i) {
      keys[i] = {out.face_ids[i], out.face_uvs[i * 2],
                 out.face_uvs[i * 2 + 1], static_cast<uint32_t>(i)};
    }
    std::sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) {
      if (a.face_id != b.face_id) return a.face_id < b.face_id;
      if (a.u != b.u) return a.u < b.u;  // NaN falls through, like Python
      if (a.v != b.v) return a.v < b.v;
      return a.index < b.index;
    });
    std::vector<int32_t> index_list(keys.size());
    for (size_t p = 0; p < keys.size(); ++p) {
      index_list[keys[p].index] = static_cast<int32_t>(p);
    }
    out.order_full.assign(index_list.begin(), index_list.end());
    out.order.assign(index_list.begin(),
                     index_list.begin() + static_cast<size_t>(out.num_curves));
  } else {
    out.order.resize(static_cast<size_t>(out.num_curves));
    std::iota(out.order.begin(), out.order.end(), 0);
    out.order_full.resize(static_cast<size_t>(out.total_records));
    std::iota(out.order_full.begin(), out.order_full.end(), 0);
  }
}

}  // namespace xgcore

#endif  // XGCORE_XGEN_PARSE_H
