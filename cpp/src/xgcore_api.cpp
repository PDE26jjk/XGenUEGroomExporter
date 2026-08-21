#include "xgcore/xgcore.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "abc_writer.h"
#include "error.h"
#include "xgen_memory.h"
#include "xgen_parse.h"

namespace {

bool valid_raw(const uint8_t* raw, size_t raw_len) {
  return raw != nullptr && raw_len > 0;
}

}  // namespace

extern "C" {

const char* xg_last_error(void) { return xgcore::last_error(); }

int xg_parse_frame_size(const uint8_t* raw, size_t raw_len, int sort_by_uv,
                        XgParseInfo* info) {
  xgcore::clear_error();
  if (!valid_raw(raw, raw_len) || info == nullptr) {
    xgcore::set_error("invalid argument: null pointer or empty input");
    return XG_ERR_INVALID_ARG;
  }
  xgcore::ParsedFrame frame;
  int rc = xgcore::parse_frame(raw, raw_len, sort_by_uv != 0, frame);
  if (rc != XG_OK) return rc;
  info->num_curves = frame.num_curves;
  info->num_cvs = frame.num_cvs;
  info->total_records = frame.total_records;
  return XG_OK;
}

int xg_parse_frame(const uint8_t* raw, size_t raw_len, int sort_by_uv,
                   float* positions, int32_t* curve_offsets, float* widths,
                   float* root_points, int32_t* index2order, XgParseInfo* info) {
  xgcore::clear_error();
  if (!valid_raw(raw, raw_len) || info == nullptr || positions == nullptr ||
      curve_offsets == nullptr || widths == nullptr || root_points == nullptr ||
      index2order == nullptr) {
    xgcore::set_error("invalid argument: null pointer or empty input");
    return XG_ERR_INVALID_ARG;
  }
  xgcore::ParsedFrame frame;
  int rc = xgcore::parse_frame(raw, raw_len, sort_by_uv != 0, frame);
  if (rc != XG_OK) return rc;

  info->num_curves = frame.num_curves;
  info->num_cvs = frame.num_cvs;
  info->total_records = frame.total_records;
  const int64_t num_curves = frame.num_curves;
  const int64_t num_cvs = frame.num_cvs;

  // Write strands in output order (sorted or raw). frame.order maps a raw
  // valid ordinal to its output index; invert it to walk outputs in order.
  std::vector<int32_t> inv(static_cast<size_t>(num_curves));
  for (int64_t i = 0; i < num_curves; ++i) {
    inv[frame.order[i]] = static_cast<int32_t>(i);
  }
  int64_t cv_cursor = 0;
  curve_offsets[0] = 0;
  for (int64_t j = 0; j < num_curves; ++j) {
    const xgcore::StrandInfo& s = frame.strands[inv[j]];
    const float* pos = frame.positions.data() + static_cast<size_t>(s.offset) * 3;
    std::memcpy(positions + cv_cursor * 3, pos, sizeof(float) * 3 * s.length);
    std::memcpy(widths + cv_cursor, frame.widths.data() + s.offset,
                sizeof(float) * s.length);
    root_points[j * 3 + 0] = pos[0];
    root_points[j * 3 + 1] = pos[1];
    root_points[j * 3 + 2] = pos[2];
    cv_cursor += static_cast<int64_t>(s.length);
    curve_offsets[j + 1] = static_cast<int32_t>(cv_cursor);
  }
  std::copy(frame.order.begin(), frame.order.end(), index2order);
  return XG_OK;
}

int xg_parse_face(const uint8_t* raw, size_t raw_len, int sort_by_uv,
                  int32_t* face_ids, float* face_uvs, int32_t* order,
                  int64_t* out_count) {
  xgcore::clear_error();
  if (!valid_raw(raw, raw_len) || out_count == nullptr) {
    xgcore::set_error("invalid argument: null pointer or empty input");
    return XG_ERR_INVALID_ARG;
  }
  if (face_ids == nullptr || face_uvs == nullptr || order == nullptr) {
    xgcore::set_error("invalid argument: null output pointer");
    return XG_ERR_INVALID_ARG;
  }
  xgcore::ParsedFrame frame;
  int rc = xgcore::parse_frame(raw, raw_len, sort_by_uv != 0, frame);
  if (rc != XG_OK) return rc;
  if (frame.face_ids.empty()) {
    xgcore::set_error("stream contains no FaceId/FaceUV data");
    return XG_ERR_MALFORMED;
  }
  const int64_t n = static_cast<int64_t>(frame.face_ids.size());
  std::copy(frame.face_ids.begin(), frame.face_ids.end(), face_ids);
  std::copy(frame.face_uvs.begin(), frame.face_uvs.end(), face_uvs);
  std::copy(frame.order_full.begin(), frame.order_full.end(), order);
  *out_count = n;
  return XG_OK;
}

int xg_abc_open(const char* path, double spf, double start_time, int animate,
                int sort_by_uv, int* out_handle) {
  xgcore::clear_error();
  if (path == nullptr || out_handle == nullptr) {
    xgcore::set_error("invalid argument: null pointer");
    return XG_ERR_INVALID_ARG;
  }
  int handle = 0;
  int rc = xgcore::abc_open(path, spf, start_time, animate != 0,
                            sort_by_uv != 0, handle);
  if (rc != XG_OK) return rc;
  *out_handle = handle;
  return XG_OK;
}

int xg_abc_add_curves(int archive_handle, const char* name, int is_guide,
                      int group_id, int* out_object_handle) {
  xgcore::clear_error();
  if (name == nullptr || out_object_handle == nullptr) {
    xgcore::set_error("invalid argument: null pointer");
    return XG_ERR_INVALID_ARG;
  }
  return xgcore::abc_add_curves(archive_handle, name, is_guide != 0, group_id,
                                *out_object_handle);
}

int xg_abc_write_first(int handle, const uint8_t* raw, size_t raw_len,
                       const char* group_name, int is_guide, int group_id) {
  xgcore::clear_error();
  if (!valid_raw(raw, raw_len) || group_name == nullptr) {
    xgcore::set_error("invalid argument: null pointer or empty input");
    return XG_ERR_INVALID_ARG;
  }
  return xgcore::abc_write_first(handle, raw, raw_len, group_name, is_guide != 0,
                                 group_id);
}

int xg_abc_write_frame(int handle, const uint8_t* raw, size_t raw_len,
                       double time) {
  xgcore::clear_error();
  if (!valid_raw(raw, raw_len)) {
    xgcore::set_error("invalid argument: null pointer or empty input");
    return XG_ERR_INVALID_ARG;
  }
  return xgcore::abc_write_frame(handle, raw, raw_len, time);
}

int xg_read_xgen_probe(uint64_t pdata_addr, int* out_ok) {
  xgcore::clear_error();
  if (out_ok == nullptr) {
    xgcore::set_error("invalid argument: null out_ok");
    return XG_ERR_INVALID_ARG;
  }
  bool ok = false;
  const int rc = xgcore::probe_memory_layout(pdata_addr, ok);
  *out_ok = ok ? 1 : 0;
  return rc;
}

int xg_abc_write_xgen_memory_first(int handle, uint64_t pdata_addr,
                                   const char* group_name, int is_guide,
                                   int group_id) {
  xgcore::clear_error();
  if (pdata_addr == 0 || group_name == nullptr) {
    xgcore::set_error("invalid argument: null pointer or zero address");
    return XG_ERR_INVALID_ARG;
  }
  return xgcore::abc_write_memory_first(handle, pdata_addr, group_name,
                                        is_guide != 0, group_id);
}

int xg_abc_write_xgen_memory_frame(int handle, uint64_t pdata_addr,
                                   double time) {
  xgcore::clear_error();
  if (pdata_addr == 0) {
    xgcore::set_error("invalid argument: zero address");
    return XG_ERR_INVALID_ARG;
  }
  return xgcore::abc_write_memory_frame(handle, pdata_addr, time);
}

int xg_abc_write_curves_first(int object_handle, const float* positions,
                              const int32_t* nverts, const int32_t* orders,
                              const float* knots, const float* widths,
                              int64_t num_curves, int64_t num_cvs,
                              int64_t knots_count) {
  xgcore::clear_error();
  if (positions == nullptr || nverts == nullptr || orders == nullptr ||
      num_curves <= 0 || num_cvs <= 0) {
    xgcore::set_error("invalid argument: null pointer or empty input");
    return XG_ERR_INVALID_ARG;
  }
  return xgcore::abc_write_curves_first(
      object_handle, positions, nverts, orders, knots, widths, num_curves,
      num_cvs, knots_count);
}

int xg_abc_write_curves_frame(int object_handle, const float* positions,
                              int64_t num_cvs) {
  xgcore::clear_error();
  if (positions == nullptr || num_cvs <= 0) {
    xgcore::set_error("invalid argument: null pointer or empty input");
    return XG_ERR_INVALID_ARG;
  }
  return xgcore::abc_write_curves_frame(object_handle, positions, num_cvs);
}

int xg_abc_write_arb_string(int object_handle, const char* name,
                            const char* const* values, int64_t count) {
  xgcore::clear_error();
  if (name == nullptr || values == nullptr || count <= 0) {
    xgcore::set_error("invalid argument: null pointer or empty input");
    return XG_ERR_INVALID_ARG;
  }
  return xgcore::abc_write_arb_string(object_handle, name, values, count);
}

int xg_abc_write_arb_int32(int object_handle, const char* name,
                           const int32_t* values, int64_t count) {
  xgcore::clear_error();
  if (name == nullptr || values == nullptr || count <= 0) {
    xgcore::set_error("invalid argument: null pointer or empty input");
    return XG_ERR_INVALID_ARG;
  }
  return xgcore::abc_write_arb_int32(object_handle, name, values, count);
}

int xg_abc_write_arb_float(int object_handle, const char* name,
                           const float* values, int64_t count) {
  xgcore::clear_error();
  if (name == nullptr || values == nullptr || count <= 0) {
    xgcore::set_error("invalid argument: null pointer or empty input");
    return XG_ERR_INVALID_ARG;
  }
  return xgcore::abc_write_arb_float(object_handle, name, values, count);
}

int xg_abc_write_arb_v2f(int object_handle, const char* name,
                         const float* values, int64_t count) {
  xgcore::clear_error();
  if (name == nullptr || values == nullptr || count <= 0) {
    xgcore::set_error("invalid argument: null pointer or empty input");
    return XG_ERR_INVALID_ARG;
  }
  return xgcore::abc_write_arb_v2f(object_handle, name, values, count);
}

int xg_abc_write_root_uv(int handle, const float* uv, size_t count) {
  xgcore::clear_error();
  if (uv == nullptr || count == 0) {
    xgcore::set_error("invalid argument: null pointer or empty input");
    return XG_ERR_INVALID_ARG;
  }
  return xgcore::abc_write_root_uv(handle, uv, count);
}

int xg_abc_close(int handle) {
  xgcore::clear_error();
  return xgcore::abc_close(handle);
}

}  // extern "C"
