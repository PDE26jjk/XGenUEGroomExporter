#ifndef XGCORE_XGEN_PARSE_H
#define XGCORE_XGEN_PARSE_H

#include <cstddef>
#include <cstdint>
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
};

// Parses one frame of raw XGen spline data and optionally sorts strands by
// (faceId, u, v). Returns XG_OK or an XG_ERR_* code.
int parse_frame(const uint8_t* raw, size_t raw_len, bool sort_by_uv,
                ParsedFrame& out);

}  // namespace xgcore

#endif  // XGCORE_XGEN_PARSE_H
