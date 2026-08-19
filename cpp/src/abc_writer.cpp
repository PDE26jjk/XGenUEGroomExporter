#include "abc_writer.h"

#include <atomic>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <Imath/ImathVec.h>

#include <Alembic/Abc/All.h>
#include <Alembic/Abc/OTypedArrayProperty.h>
#include <Alembic/Abc/TypedArraySample.h>
#include <Alembic/AbcCoreOgawa/All.h>
#include <Alembic/AbcGeom/All.h>

#include "error.h"
#include "xgen_parse.h"
#include "xgcore/xgcore.h"

namespace Abc = Alembic::Abc;
namespace AbcGeom = Alembic::AbcGeom;
namespace AbcA = Alembic::AbcCoreAbstract;
namespace AbcCoreOgawa = Alembic::AbcCoreOgawa;

namespace xgcore {
namespace {

std::atomic<int> g_next_handle{1};
std::mutex g_handles_mutex;
std::map<int, std::unique_ptr<AbcWriter>> g_handles;

std::string exception_message() {
  try {
    throw;
  } catch (const std::exception& e) {
    return e.what();
  } catch (...) {
    return "unknown exception";
  }
}

int alembic_error(const std::string& what) {
  set_error("alembic: " + what);
  return XG_ERR_ALEMBIC;
}

int register_writer(std::unique_ptr<AbcWriter> writer, int& out_handle) {
  std::lock_guard<std::mutex> lock(g_handles_mutex);
  out_handle = g_next_handle++;
  g_handles[out_handle] = std::move(writer);
  return XG_OK;
}

AbcWriter* lookup_writer(int handle) {
  std::lock_guard<std::mutex> lock(g_handles_mutex);
  const auto it = g_handles.find(handle);
  return it == g_handles.end() ? nullptr : it->second.get();
}

// Fills output-ordered positions (one V3f per CV) and widths using the
// frame's permutation (identity when sorting is disabled).
void build_output_arrays(const ParsedFrame& frame,
                         std::vector<Imath::V3f>& positions,
                         std::vector<float>& widths) {
  const int64_t n = frame.num_curves;
  std::vector<int32_t> inv(static_cast<size_t>(n));
  for (int64_t i = 0; i < n; ++i) {
    inv[frame.order[i]] = static_cast<int32_t>(i);
  }
  positions.resize(static_cast<size_t>(frame.num_cvs));
  widths.resize(static_cast<size_t>(frame.num_cvs));
  size_t cursor = 0;
  for (int64_t j = 0; j < n; ++j) {
    const StrandInfo& s = frame.strands[inv[j]];
    for (uint64_t k = 0; k < s.length; ++k) {
      const float* p =
          frame.positions.data() + (static_cast<size_t>(s.offset) + k) * 3;
      positions[cursor] = Imath::V3f(p[0], p[1], p[2]);
      widths[cursor] = frame.widths[static_cast<size_t>(s.offset) + k];
      ++cursor;
    }
  }
}

}  // namespace

// Opaque in abc_writer.h; defined here at xgcore scope.
struct AbcWriter {
  Abc::OArchive archive;
  AbcGeom::OCurves curves;
  AbcGeom::OCurvesSchema schema;
  bool animate = false;
  bool sort_by_uv = false;
  uint32_t time_index = 0;
  bool first_written = false;
  int64_t num_curves = 0;
  int64_t num_cvs = 0;
  std::vector<int32_t> nverts;
  std::vector<uint8_t> orders;
  std::vector<float> knots;
  std::vector<float> widths;
};

int abc_open(const char* path, double spf, double start_time, bool animate,
             bool sort_by_uv, int& out_handle) {
  try {
    auto writer = std::make_unique<AbcWriter>();
    writer->archive = Abc::OArchive(AbcCoreOgawa::WriteArchive(), path);
    writer->animate = animate;
    writer->sort_by_uv = sort_by_uv;
    if (animate) {
      writer->time_index = writer->archive.addTimeSampling(
          AbcA::TimeSampling(spf, start_time));
    }
    return register_writer(std::move(writer), out_handle);
  } catch (...) {
    return alembic_error(exception_message());
  }
}

int abc_write_first(int handle, const uint8_t* raw, size_t raw_len,
                    const char* group_name, bool is_guide, int group_id) {
  AbcWriter* w = lookup_writer(handle);
  if (w == nullptr) {
    set_error("invalid archive handle");
    return XG_ERR_INVALID_ARG;
  }
  if (w->first_written) {
    set_error("first frame already written");
    return XG_ERR_STATE;
  }
  ParsedFrame frame;
  int rc = parse_frame(raw, raw_len, w->sort_by_uv, frame);
  if (rc != XG_OK) return rc;
  if (frame.num_curves == 0) {
    set_error("no valid strands in the first frame");
    return XG_ERR_MALFORMED;
  }

  try {
    Abc::OObject top = w->archive.getTop();
    if (w->animate) {
      w->curves = AbcGeom::OCurves(top, group_name, w->time_index);
    } else {
      w->curves = AbcGeom::OCurves(top, group_name);
    }
    w->schema = w->curves.getSchema();

    const int64_t n = frame.num_curves;
    std::vector<int32_t> nverts(static_cast<size_t>(n));
    std::vector<uint8_t> orders(static_cast<size_t>(n), 4);
    std::vector<float> knots;
    for (int64_t i = 0; i < n; ++i) {
      const int64_t length = static_cast<int64_t>(frame.strands[i].length);
      nverts[static_cast<size_t>(i)] = static_cast<int32_t>(length);
      // Cubic B-spline knots with degree 3, exactly like the Python exporter:
      // [0,0,0] + [0..inside-1] + [inside-1,inside-1,inside-1].
      const int64_t inside = length - 3 + 1;
      for (int k = 0; k < 3; ++k) knots.push_back(0.0f);
      for (int64_t k = 0; k < inside; ++k) {
        knots.push_back(static_cast<float>(k));
      }
      for (int k = 0; k < 3; ++k) {
        knots.push_back(static_cast<float>(inside - 1));
      }
    }

    std::vector<Imath::V3f> positions;
    std::vector<float> widths_out;
    build_output_arrays(frame, positions, widths_out);

    AbcGeom::OCurvesSchema::Sample sample;
    sample.setBasis(AbcGeom::kBsplineBasis);
    sample.setWrap(AbcGeom::kNonPeriodic);
    sample.setType(AbcGeom::kCubic);
    sample.setCurvesNumVertices(Abc::Int32ArraySample(nverts));
    sample.setOrders(Abc::UcharArraySample(orders));
    sample.setKnots(Abc::FloatArraySample(knots));
    sample.setPositions(Abc::P3fArraySample(positions));
    sample.setWidths(AbcGeom::OFloatGeomParam::Sample(
        Abc::FloatArraySample(widths_out), AbcGeom::kVertexScope));
    w->schema.set(sample);

    // Arbitrary geometry parameters expected by UE Groom.
    Abc::OCompoundProperty arb = w->schema.getArbGeomParams();
    Abc::OStringArrayProperty name_prop(arb, "groom_group_name");
    std::vector<std::string> names{group_name};
    name_prop.set(Abc::StringArraySample(names));
    if (is_guide) {
      Abc::OInt16ArrayProperty guide_prop(arb, "groom_guide");
      std::vector<int16_t> guide{1};
      guide_prop.set(Abc::Int16ArraySample(guide));
    }
    Abc::OInt32ArrayProperty group_id_prop(arb, "groom_group_id");
    std::vector<int32_t> group_ids{group_id};
    group_id_prop.set(Abc::Int32ArraySample(group_ids));

    w->num_curves = n;
    w->num_cvs = frame.num_cvs;
    w->nverts = std::move(nverts);
    w->orders = std::move(orders);
    w->knots = std::move(knots);
    w->widths = std::move(widths_out);
    w->first_written = true;
    return XG_OK;
  } catch (...) {
    return alembic_error(exception_message());
  }
}

int abc_write_frame(int handle, const uint8_t* raw, size_t raw_len,
                    double time) {
  (void)time;  // sample times come from the archive's time sampling
  AbcWriter* w = lookup_writer(handle);
  if (w == nullptr) {
    set_error("invalid archive handle");
    return XG_ERR_INVALID_ARG;
  }
  if (!w->first_written) {
    set_error("first frame not written");
    return XG_ERR_STATE;
  }
  ParsedFrame frame;
  int rc = parse_frame(raw, raw_len, w->sort_by_uv, frame);
  if (rc != XG_OK) return rc;
  if (frame.num_curves != w->num_curves || frame.num_cvs != w->num_cvs) {
    set_error("frame vertex count does not match the first frame");
    return XG_ERR_STATE;
  }

  try {
    std::vector<Imath::V3f> positions;
    std::vector<float> widths;
    build_output_arrays(frame, positions, widths);

    AbcGeom::OCurvesSchema::Sample sample;
    sample.setBasis(AbcGeom::kBsplineBasis);
    sample.setWrap(AbcGeom::kNonPeriodic);
    sample.setType(AbcGeom::kCubic);
    sample.setCurvesNumVertices(Abc::Int32ArraySample(w->nverts));
    sample.setOrders(Abc::UcharArraySample(w->orders));
    sample.setKnots(Abc::FloatArraySample(w->knots));
    sample.setWidths(AbcGeom::OFloatGeomParam::Sample(
        Abc::FloatArraySample(w->widths), AbcGeom::kVertexScope));
    sample.setPositions(Abc::P3fArraySample(positions));
    w->schema.set(sample);
    return XG_OK;
  } catch (...) {
    return alembic_error(exception_message());
  }
}

int abc_write_root_uv(int handle, const float* uv, size_t count) {
  AbcWriter* w = lookup_writer(handle);
  if (w == nullptr) {
    set_error("invalid archive handle");
    return XG_ERR_INVALID_ARG;
  }
  if (!w->first_written) {
    set_error("first frame not written");
    return XG_ERR_STATE;
  }
  if (static_cast<int64_t>(count) != w->num_curves) {
    set_error("root UV count does not match the number of curves");
    return XG_ERR_INVALID_ARG;
  }
  try {
    std::vector<Imath::V2f> uvs(count);
    for (size_t i = 0; i < count; ++i) {
      uvs[i] = Imath::V2f(uv[2 * i], uv[2 * i + 1]);
    }
    Abc::OCompoundProperty arb = w->schema.getArbGeomParams();
    Abc::OV2fArrayProperty prop(arb, "groom_root_uv");
    prop.set(Abc::V2fArraySample(uvs));
    return XG_OK;
  } catch (...) {
    return alembic_error(exception_message());
  }
}

int abc_close(int handle) {
  std::lock_guard<std::mutex> lock(g_handles_mutex);
  const auto it = g_handles.find(handle);
  if (it == g_handles.end()) {
    set_error("invalid archive handle");
    return XG_ERR_INVALID_ARG;
  }
  g_handles.erase(it);  // destroys the writer; the archive finalizes the file
  return XG_OK;
}

}  // namespace xgcore
