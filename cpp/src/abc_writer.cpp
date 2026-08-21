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
#include "xgen_memory.h"
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

// An open Alembic archive and its implicit single-object marker.
struct AbcArchive {
  Abc::OArchive archive;
  bool animate = false;
  bool sort_by_uv = false;
  uint32_t time_index = 0;
  int implicit_object = 0;  // object created by the raw single-object API
};

// A top-level OCurves object inside an archive.
struct AbcObject {
  int archive_handle = 0;
  AbcGeom::OCurves curves;
  AbcGeom::OCurvesSchema schema;
  bool first_written = false;
  bool has_group_name = false;
  int64_t num_curves = 0;
  int64_t num_cvs = 0;
  std::vector<int32_t> nverts;
  std::vector<uint8_t> orders;
  std::vector<float> knots;
  std::vector<float> widths;
};

namespace {

struct Entry {
  enum class Kind { Archive, Object } kind;
  std::unique_ptr<AbcArchive> archive;
  std::unique_ptr<AbcObject> object;
};

std::map<int, std::unique_ptr<Entry>> g_handles;

int register_archive(std::unique_ptr<AbcArchive> archive, int& out_handle) {
  std::lock_guard<std::mutex> lock(g_handles_mutex);
  out_handle = g_next_handle++;
  auto entry = std::make_unique<Entry>();
  entry->kind = Entry::Kind::Archive;
  entry->archive = std::move(archive);
  g_handles[out_handle] = std::move(entry);
  return XG_OK;
}

int register_object(std::unique_ptr<AbcObject> object, int& out_handle) {
  std::lock_guard<std::mutex> lock(g_handles_mutex);
  out_handle = g_next_handle++;
  auto entry = std::make_unique<Entry>();
  entry->kind = Entry::Kind::Object;
  entry->object = std::move(object);
  g_handles[out_handle] = std::move(entry);
  return XG_OK;
}

AbcObject* lookup_object(int handle) {
  std::lock_guard<std::mutex> lock(g_handles_mutex);
  const auto it = g_handles.find(handle);
  if (it == g_handles.end() || it->second->kind != Entry::Kind::Object) {
    return nullptr;
  }
  return it->second->object.get();
}

// Resolves an object handle, or an archive handle that already has its lazy
// implicit object. Sets the thread error and returns nullptr on failure.
AbcObject* resolve_object(int handle) {
  AbcObject* obj = lookup_object(handle);
  if (obj != nullptr) return obj;
  std::lock_guard<std::mutex> lock(g_handles_mutex);
  const auto it = g_handles.find(handle);
  if (it == g_handles.end() || it->second->kind != Entry::Kind::Archive) {
    set_error("invalid archive/object handle");
    return nullptr;
  }
  const int implicit = it->second->archive->implicit_object;
  if (implicit == 0) {
    set_error("no curve object created on this archive");
    return nullptr;
  }
  return g_handles[implicit]->object.get();
}

// Lazily creates the single implicit object for the backward-compatible raw
// API and returns it. Fails if explicit objects were already added.
int ensure_implicit_object(int archive_handle, const char* group_name,
                           bool is_guide, int group_id, AbcObject*& out) {
  out = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_handles_mutex);
    const auto it = g_handles.find(archive_handle);
    if (it == g_handles.end() || it->second->kind != Entry::Kind::Archive) {
      set_error("invalid archive handle");
      return XG_ERR_INVALID_ARG;
    }
    AbcArchive* a = it->second->archive.get();
    if (a->implicit_object != 0) {
      out = g_handles[a->implicit_object]->object.get();
      return XG_OK;
    }
  }
  int new_handle = 0;
  int rc = abc_add_curves(archive_handle, group_name, is_guide, group_id,
                          new_handle);
  if (rc != XG_OK) return rc;
  {
    std::lock_guard<std::mutex> lock(g_handles_mutex);
    const auto it = g_handles.find(archive_handle);
    if (it == g_handles.end() || it->second->kind != Entry::Kind::Archive) {
      set_error("invalid archive handle");
      return XG_ERR_INVALID_ARG;
    }
    it->second->archive->implicit_object = new_handle;
    out = g_handles[new_handle]->object.get();
  }
  return XG_OK;
}

}  // namespace

int abc_open(const char* path, double spf, double start_time, bool animate,
             bool sort_by_uv, int& out_handle) {
  try {
    auto archive = std::make_unique<AbcArchive>();
    archive->archive = Abc::OArchive(AbcCoreOgawa::WriteArchive(), path);
    archive->animate = animate;
    archive->sort_by_uv = sort_by_uv;
    if (animate) {
      archive->time_index = archive->archive.addTimeSampling(
          AbcA::TimeSampling(spf, start_time));
    }
    return register_archive(std::move(archive), out_handle);
  } catch (...) {
    return alembic_error(exception_message());
  }
}

int abc_add_curves(int archive_handle, const char* name, bool is_guide,
                   int group_id, int& out_object_handle) {
  if (name == nullptr) {
    set_error("invalid argument: null object name");
    return XG_ERR_INVALID_ARG;
  }
  try {
    AbcArchive* archive = nullptr;
    {
      std::lock_guard<std::mutex> lock(g_handles_mutex);
      const auto it = g_handles.find(archive_handle);
      if (it == g_handles.end() || it->second->kind != Entry::Kind::Archive) {
        set_error("invalid archive handle");
        return XG_ERR_INVALID_ARG;
      }
      archive = it->second->archive.get();
      if (archive->implicit_object != 0) {
        set_error("cannot add explicit objects after the single-object API");
        return XG_ERR_STATE;
      }
    }

    auto obj = std::make_unique<AbcObject>();
    obj->archive_handle = archive_handle;
    Abc::OObject top = archive->archive.getTop();
    if (archive->animate) {
      obj->curves = AbcGeom::OCurves(top, name, archive->time_index);
    } else {
      obj->curves = AbcGeom::OCurves(top, name);
    }
    obj->schema = obj->curves.getSchema();

    // Arbitrary geometry parameters expected by UE Groom. groom_group_name is
    // written by the caller (write_group_name or the raw first-frame path) so
    // the object name and the group name can differ, as in the Python
    // exporters. groom_group_id is only written when group_id >= 0 so a later
    // group-id pass can write it.
    Abc::OCompoundProperty arb = obj->schema.getArbGeomParams();
    if (is_guide) {
      Abc::OInt16ArrayProperty guide_prop(arb, "groom_guide");
      std::vector<int16_t> guide{1};
      guide_prop.set(Abc::Int16ArraySample(guide));
    }
    if (group_id >= 0) {
      Abc::OInt32ArrayProperty group_id_prop(arb, "groom_group_id");
      std::vector<int32_t> ids{group_id};
      group_id_prop.set(Abc::Int32ArraySample(ids));
    }

    return register_object(std::move(obj), out_object_handle);
  } catch (...) {
    return alembic_error(exception_message());
  }
}

namespace {

// Shared first-sample writer used by both the serialized and direct-memory
// paths: stores topology (nverts/orders/knots/widths) plus first positions.
int write_first_sample(AbcObject* w, const ParsedFrame& frame,
                       bool implicit_object, const char* group_name) {
  try {
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

    if (implicit_object && !w->has_group_name) {
      if (group_name == nullptr) {
        set_error("invalid argument: null group name");
        return XG_ERR_INVALID_ARG;
      }
      Abc::OCompoundProperty arb = w->schema.getArbGeomParams();
      Abc::OStringArrayProperty name_prop(arb, "groom_group_name");
      std::vector<std::string> names{group_name};
      name_prop.set(Abc::StringArraySample(names));
      w->has_group_name = true;
    }

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

// Shared subsequent-sample writer used by both read paths.
int write_frame_sample(AbcObject* w, const ParsedFrame& frame) {
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

}  // namespace

int abc_write_first(int handle, const uint8_t* raw, size_t raw_len,
                    const char* group_name, bool is_guide, int group_id) {
  AbcObject* w = lookup_object(handle);
  const bool implicit_object = w == nullptr;
  if (implicit_object) {
    int rc = ensure_implicit_object(handle, group_name, is_guide, group_id, w);
    if (rc != XG_OK) return rc;
  }
  if (w->first_written) {
    set_error("first frame already written");
    return XG_ERR_STATE;
  }
  const AbcArchive* archive = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_handles_mutex);
    const auto it = g_handles.find(w->archive_handle);
    if (it == g_handles.end()) {
      set_error("invalid archive handle");
      return XG_ERR_INVALID_ARG;
    }
    archive = it->second->archive.get();
  }
  ParsedFrame frame;
  int rc = parse_frame(raw, raw_len, archive->sort_by_uv, frame);
  if (rc != XG_OK) return rc;
  if (frame.num_curves == 0) {
    set_error("no valid strands in the first frame");
    return XG_ERR_MALFORMED;
  }
  return write_first_sample(w, frame, implicit_object, group_name);
}

int abc_write_frame(int handle, const uint8_t* raw, size_t raw_len,
                    double time) {
  (void)time;  // sample times come from the archive's time sampling
  AbcObject* w = resolve_object(handle);
  if (w == nullptr) return XG_ERR_INVALID_ARG;
  if (!w->first_written) {
    set_error("first frame not written");
    return XG_ERR_STATE;
  }
  const AbcArchive* archive = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_handles_mutex);
    const auto it = g_handles.find(w->archive_handle);
    if (it == g_handles.end()) {
      set_error("invalid archive handle");
      return XG_ERR_INVALID_ARG;
    }
    archive = it->second->archive.get();
  }
  ParsedFrame frame;
  int rc = parse_frame(raw, raw_len, archive->sort_by_uv, frame);
  if (rc != XG_OK) return rc;
  if (frame.num_curves != w->num_curves || frame.num_cvs != w->num_cvs) {
    set_error("frame vertex count does not match the first frame");
    return XG_ERR_STATE;
  }
  return write_frame_sample(w, frame);
}

int abc_write_memory_first(int handle, uint64_t pdata_addr,
                           const char* group_name, bool is_guide,
                           int group_id) {
  AbcObject* w = lookup_object(handle);
  const bool implicit_object = w == nullptr;
  if (implicit_object) {
    int rc = ensure_implicit_object(handle, group_name, is_guide, group_id, w);
    if (rc != XG_OK) return rc;
  }
  if (w->first_written) {
    set_error("first frame already written");
    return XG_ERR_STATE;
  }
  const AbcArchive* archive = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_handles_mutex);
    const auto it = g_handles.find(w->archive_handle);
    if (it == g_handles.end()) {
      set_error("invalid archive handle");
      return XG_ERR_INVALID_ARG;
    }
    archive = it->second->archive.get();
  }
  ParsedFrame frame;
  int rc = parse_memory_frame(pdata_addr, archive->sort_by_uv, frame);
  if (rc != XG_OK) return rc;
  if (frame.num_curves == 0) {
    set_error("no valid strands in the first frame");
    return XG_ERR_MALFORMED;
  }
  return write_first_sample(w, frame, implicit_object, group_name);
}

int abc_write_memory_frame(int handle, uint64_t pdata_addr, double time) {
  (void)time;  // sample times come from the archive's time sampling
  AbcObject* w = resolve_object(handle);
  if (w == nullptr) return XG_ERR_INVALID_ARG;
  if (!w->first_written) {
    set_error("first frame not written");
    return XG_ERR_STATE;
  }
  const AbcArchive* archive = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_handles_mutex);
    const auto it = g_handles.find(w->archive_handle);
    if (it == g_handles.end()) {
      set_error("invalid archive handle");
      return XG_ERR_INVALID_ARG;
    }
    archive = it->second->archive.get();
  }
  ParsedFrame frame;
  int rc = parse_memory_frame(pdata_addr, archive->sort_by_uv, frame);
  if (rc != XG_OK) return rc;
  if (frame.num_curves != w->num_curves || frame.num_cvs != w->num_cvs) {
    set_error("frame vertex count does not match the first frame");
    return XG_ERR_STATE;
  }
  return write_frame_sample(w, frame);
}

int abc_write_curves_first(int object_handle, const float* positions,
                           const int32_t* nverts, const int32_t* orders,
                           const float* knots, const float* widths,
                           int64_t num_curves, int64_t num_cvs,
                           int64_t knots_count) {
  AbcObject* w = lookup_object(object_handle);
  if (w == nullptr) {
    set_error("invalid object handle");
    return XG_ERR_INVALID_ARG;
  }
  if (w->first_written) {
    set_error("first frame already written");
    return XG_ERR_STATE;
  }
  if (positions == nullptr || nverts == nullptr || orders == nullptr ||
      num_curves <= 0 || num_cvs <= 0) {
    set_error("invalid argument: null pointer or empty input");
    return XG_ERR_INVALID_ARG;
  }
  if (knots_count > 0 && knots == nullptr) {
    set_error("invalid argument: null knots");
    return XG_ERR_INVALID_ARG;
  }

  try {
    const int64_t n = num_curves;
    std::vector<int32_t> nverts_v(static_cast<size_t>(n));
    std::vector<uint8_t> orders_v(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) {
      nverts_v[static_cast<size_t>(i)] = nverts[i];
      orders_v[static_cast<size_t>(i)] = static_cast<uint8_t>(orders[i]);
    }
    std::vector<float> knots_v(knots, knots + knots_count);
    std::vector<Imath::V3f> positions_v(static_cast<size_t>(num_cvs));
    for (int64_t i = 0; i < num_cvs; ++i) {
      positions_v[static_cast<size_t>(i)] =
          Imath::V3f(positions[3 * i], positions[3 * i + 1],
                     positions[3 * i + 2]);
    }

    AbcGeom::OCurvesSchema::Sample sample;
    sample.setBasis(AbcGeom::kBsplineBasis);
    sample.setWrap(AbcGeom::kNonPeriodic);
    sample.setType(orders_v[0] == 2 ? AbcGeom::kLinear : AbcGeom::kCubic);
    sample.setCurvesNumVertices(Abc::Int32ArraySample(nverts_v));
    sample.setOrders(Abc::UcharArraySample(orders_v));
    sample.setKnots(Abc::FloatArraySample(knots_v));
    sample.setPositions(Abc::P3fArraySample(positions_v));
    if (widths != nullptr) {
      std::vector<float> widths_v(widths, widths + num_cvs);
      sample.setWidths(AbcGeom::OFloatGeomParam::Sample(
          Abc::FloatArraySample(widths_v), AbcGeom::kVertexScope));
    }
    w->schema.set(sample);

    w->num_curves = n;
    w->num_cvs = num_cvs;
    w->nverts = std::move(nverts_v);
    w->orders = std::move(orders_v);
    w->knots = std::move(knots_v);
    if (widths != nullptr) {
      w->widths.assign(widths, widths + num_cvs);
    } else {
      w->widths.clear();
    }
    w->first_written = true;
    return XG_OK;
  } catch (...) {
    return alembic_error(exception_message());
  }
}

int abc_write_curves_frame(int object_handle, const float* positions,
                           int64_t num_cvs) {
  AbcObject* w = lookup_object(object_handle);
  if (w == nullptr) {
    set_error("invalid object handle");
    return XG_ERR_INVALID_ARG;
  }
  if (!w->first_written) {
    set_error("first frame not written");
    return XG_ERR_STATE;
  }
  if (positions == nullptr || num_cvs != w->num_cvs) {
    set_error("frame vertex count does not match the first frame");
    return XG_ERR_STATE;
  }

  try {
    std::vector<Imath::V3f> positions_v(static_cast<size_t>(num_cvs));
    for (int64_t i = 0; i < num_cvs; ++i) {
      positions_v[static_cast<size_t>(i)] =
          Imath::V3f(positions[3 * i], positions[3 * i + 1],
                     positions[3 * i + 2]);
    }

    AbcGeom::OCurvesSchema::Sample sample;
    sample.setBasis(AbcGeom::kBsplineBasis);
    sample.setWrap(AbcGeom::kNonPeriodic);
    sample.setType(w->orders[0] == 2 ? AbcGeom::kLinear : AbcGeom::kCubic);
    sample.setCurvesNumVertices(Abc::Int32ArraySample(w->nverts));
    sample.setOrders(Abc::UcharArraySample(w->orders));
    sample.setKnots(Abc::FloatArraySample(w->knots));
    if (!w->widths.empty()) {
      sample.setWidths(AbcGeom::OFloatGeomParam::Sample(
          Abc::FloatArraySample(w->widths), AbcGeom::kVertexScope));
    }
    sample.setPositions(Abc::P3fArraySample(positions_v));
    w->schema.set(sample);
    return XG_OK;
  } catch (...) {
    return alembic_error(exception_message());
  }
}

int abc_write_arb_string(int object_handle, const char* name,
                         const char* const* values, int64_t count) {
  AbcObject* w = lookup_object(object_handle);
  if (w == nullptr) {
    set_error("invalid object handle");
    return XG_ERR_INVALID_ARG;
  }
  if (name == nullptr || values == nullptr || count <= 0) {
    set_error("invalid argument: null pointer or empty input");
    return XG_ERR_INVALID_ARG;
  }
  try {
    std::vector<std::string> data(static_cast<size_t>(count));
    for (int64_t i = 0; i < count; ++i) {
      if (values[i] == nullptr) {
        set_error("invalid argument: null string value");
        return XG_ERR_INVALID_ARG;
      }
      data[static_cast<size_t>(i)] = values[i];
    }
    Abc::OCompoundProperty arb = w->schema.getArbGeomParams();
    Abc::OStringArrayProperty prop(arb, name);
    prop.set(Abc::StringArraySample(data));
    return XG_OK;
  } catch (...) {
    return alembic_error(exception_message());
  }
}

int abc_write_arb_int32(int object_handle, const char* name,
                        const int32_t* values, int64_t count) {
  AbcObject* w = lookup_object(object_handle);
  if (w == nullptr) {
    set_error("invalid object handle");
    return XG_ERR_INVALID_ARG;
  }
  if (name == nullptr || values == nullptr || count <= 0) {
    set_error("invalid argument: null pointer or empty input");
    return XG_ERR_INVALID_ARG;
  }
  try {
    std::vector<int32_t> data(static_cast<size_t>(count));
    for (int64_t i = 0; i < count; ++i) {
      data[static_cast<size_t>(i)] = values[i];
    }
    Abc::OCompoundProperty arb = w->schema.getArbGeomParams();
    Abc::OInt32ArrayProperty prop(arb, name);
    prop.set(Abc::Int32ArraySample(data));
    return XG_OK;
  } catch (...) {
    return alembic_error(exception_message());
  }
}

int abc_write_arb_float(int object_handle, const char* name,
                        const float* values, int64_t count) {
  AbcObject* w = lookup_object(object_handle);
  if (w == nullptr) {
    set_error("invalid object handle");
    return XG_ERR_INVALID_ARG;
  }
  if (name == nullptr || values == nullptr || count <= 0) {
    set_error("invalid argument: null pointer or empty input");
    return XG_ERR_INVALID_ARG;
  }
  try {
    std::vector<float> data(static_cast<size_t>(count));
    for (int64_t i = 0; i < count; ++i) {
      data[static_cast<size_t>(i)] = values[i];
    }
    Abc::OCompoundProperty arb = w->schema.getArbGeomParams();
    Abc::OFloatArrayProperty prop(arb, name);
    prop.set(Abc::FloatArraySample(data));
    return XG_OK;
  } catch (...) {
    return alembic_error(exception_message());
  }
}

int abc_write_arb_v2f(int object_handle, const char* name,
                      const float* values, int64_t count) {
  AbcObject* w = lookup_object(object_handle);
  if (w == nullptr) {
    set_error("invalid object handle");
    return XG_ERR_INVALID_ARG;
  }
  if (name == nullptr || values == nullptr || count <= 0) {
    set_error("invalid argument: null pointer or empty input");
    return XG_ERR_INVALID_ARG;
  }
  try {
    std::vector<Imath::V2f> data(static_cast<size_t>(count));
    for (int64_t i = 0; i < count; ++i) {
      data[static_cast<size_t>(i)] =
          Imath::V2f(values[2 * i], values[2 * i + 1]);
    }
    Abc::OCompoundProperty arb = w->schema.getArbGeomParams();
    Abc::OV2fArrayProperty prop(arb, name);
    prop.set(Abc::V2fArraySample(data));
    return XG_OK;
  } catch (...) {
    return alembic_error(exception_message());
  }
}

int abc_write_root_uv(int handle, const float* uv, size_t count) {
  AbcObject* w = resolve_object(handle);
  if (w == nullptr) return XG_ERR_INVALID_ARG;
  if (!w->first_written) {
    set_error("first frame not written");
    return XG_ERR_STATE;
  }
  if (uv == nullptr || count == 0) {
    set_error("invalid argument: null pointer or empty input");
    return XG_ERR_INVALID_ARG;
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
  if (it == g_handles.end() || it->second->kind != Entry::Kind::Archive) {
    set_error("invalid archive handle");
    return XG_ERR_INVALID_ARG;
  }
  for (auto entry = g_handles.begin(); entry != g_handles.end();) {
    if (entry->first == handle ||
        (entry->second->kind == Entry::Kind::Object &&
         entry->second->object->archive_handle == handle)) {
      entry = g_handles.erase(entry);
    } else {
      ++entry;
    }
  }
  return XG_OK;
}

}  // namespace xgcore
