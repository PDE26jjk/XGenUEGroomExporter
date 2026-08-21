#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#include <Alembic/Abc/All.h>
#include <Alembic/AbcCoreOgawa/All.h>
#include <Alembic/AbcGeom/All.h>

#include "xgcore/xgcore.h"

namespace Abc = Alembic::Abc;
namespace AbcA = Alembic::AbcCoreAbstract;
namespace AbcCoreOgawa = Alembic::AbcCoreOgawa;
namespace AbcGeom = Alembic::AbcGeom;

namespace {

void print_usage() {
  std::printf(
      "xgcore_test - xgcore CLI\n"
      "usage:\n"
      "  xgcore_test version\n"
      "  xgcore_test parse <raw.bin> [--sort] [--dump-dir <dir>]\n"
      "  xgcore_test read <file.abc>\n"
      "  xgcore_test abc <out.abc> <raw1.bin> [raw2.bin ...] [options]\n"
      "  xgcore_test multi <out.abc> <raw1.bin> [raw2.bin ...]"
      " --object <name> [--object <name> ...] [options]\n"
      "options for abc:\n"
      "  --animate --start <frame> --fps <fps>\n"
      "  --group <name> [--guide] [--group-id <id>]\n");
}

bool read_file(const char* path, std::vector<uint8_t>& out) {
  FILE* f = std::fopen(path, "rb");
  if (f == nullptr) {
    std::fprintf(stderr, "cannot open %s\n", path);
    return false;
  }
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  out.resize(static_cast<size_t>(size));
  const bool ok = std::fread(out.data(), 1, out.size(), f) == out.size();
  std::fclose(f);
  if (!ok) std::fprintf(stderr, "failed to read %s\n", path);
  return ok;
}

bool write_file(const char* path, const void* data, size_t size) {
  FILE* f = std::fopen(path, "wb");
  if (f == nullptr) {
    std::fprintf(stderr, "cannot write %s\n", path);
    return false;
  }
  const bool ok = std::fwrite(data, 1, size, f) == size;
  std::fclose(f);
  if (!ok) std::fprintf(stderr, "failed to write %s\n", path);
  return ok;
}

int cmd_parse(int argc, char** argv) {
  if (argc < 3) {
    print_usage();
    return 1;
  }
  const char* raw_path = argv[2];
  int sort_by_uv = 0;
  const char* dump_dir = nullptr;
  for (int i = 3; i < argc; ++i) {
    if (std::strcmp(argv[i], "--sort") == 0) {
      sort_by_uv = 1;
    } else if (std::strcmp(argv[i], "--dump-dir") == 0 && i + 1 < argc) {
      dump_dir = argv[++i];
    } else {
      std::fprintf(stderr, "unknown option: %s\n", argv[i]);
      return 1;
    }
  }

  std::vector<uint8_t> raw;
  if (!read_file(raw_path, raw)) return 2;

  XgParseInfo info{};
  int rc = xg_parse_frame_size(raw.data(), raw.size(), sort_by_uv, &info);
  if (rc != XG_OK) {
    std::fprintf(stderr, "xg_parse_frame_size failed (%d): %s\n", rc,
                 xg_last_error());
    return 2;
  }

  std::vector<float> positions(static_cast<size_t>(info.num_cvs) * 3);
  std::vector<int32_t> curve_offsets(static_cast<size_t>(info.num_curves) + 1);
  std::vector<float> widths(static_cast<size_t>(info.num_cvs));
  std::vector<float> root_points(static_cast<size_t>(info.num_curves) * 3);
  std::vector<int32_t> order(static_cast<size_t>(info.num_curves));
  rc = xg_parse_frame(raw.data(), raw.size(), sort_by_uv, positions.data(),
                      curve_offsets.data(), widths.data(), root_points.data(),
                      order.data(), &info);
  if (rc != XG_OK) {
    std::fprintf(stderr, "xg_parse_frame failed (%d): %s\n", rc, xg_last_error());
    return 2;
  }

  std::printf("num_curves: %lld\nnum_cvs: %lld\nsort_by_uv: %d\n",
              static_cast<long long>(info.num_curves),
              static_cast<long long>(info.num_cvs), sort_by_uv);

  if (dump_dir != nullptr) {
    std::string meta = "{\"num_curves\":" + std::to_string(info.num_curves) +
                       ",\"num_cvs\":" + std::to_string(info.num_cvs) + "}";
    if (!write_file((std::string(dump_dir) + "/meta.json").c_str(), meta.data(),
                    meta.size()))
      return 2;
    if (!write_file((std::string(dump_dir) + "/order.bin").c_str(), order.data(),
                    order.size() * sizeof(int32_t)))
      return 2;
    if (!write_file((std::string(dump_dir) + "/curve_offsets.bin").c_str(),
                    curve_offsets.data(),
                    curve_offsets.size() * sizeof(int32_t)))
      return 2;
    if (!write_file((std::string(dump_dir) + "/positions.bin").c_str(),
                    positions.data(), positions.size() * sizeof(float)))
      return 2;
    if (!write_file((std::string(dump_dir) + "/widths.bin").c_str(),
                    widths.data(), widths.size() * sizeof(float)))
      return 2;
    if (!write_file((std::string(dump_dir) + "/root_points.bin").c_str(),
                    root_points.data(), root_points.size() * sizeof(float)))
      return 2;
    std::printf("dumped parse outputs to %s\n", dump_dir);
  }
  return 0;
}

int cmd_abc(int argc, char** argv) {
  if (argc < 4) {
    print_usage();
    return 1;
  }
  const char* abc_path = argv[2];
  std::vector<std::string> raw_files;
  int argi = 3;
  while (argi < argc && argv[argi][0] != '-') {
    raw_files.emplace_back(argv[argi]);
    ++argi;
  }
  if (raw_files.empty()) {
    print_usage();
    return 1;
  }
  bool animate = false;
  bool guide = false;
  bool sort_by_uv = false;
  int start_frame = 1;
  double fps = 24.0;
  const char* group = "groom";
  int group_id = 0;
  for (int i = argi; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--animate") {
      animate = true;
    } else if (arg == "--sort") {
      sort_by_uv = true;
    } else if (arg == "--guide") {
      guide = true;
    } else if (arg == "--start" && i + 1 < argc) {
      start_frame = std::atoi(argv[++i]);
    } else if (arg == "--fps" && i + 1 < argc) {
      fps = std::atof(argv[++i]);
    } else if (arg == "--group" && i + 1 < argc) {
      group = argv[++i];
    } else if (arg == "--group-id" && i + 1 < argc) {
      group_id = std::atoi(argv[++i]);
    } else {
      std::fprintf(stderr, "unknown option: %s\n", argv[i]);
      return 1;
    }
  }

  std::vector<std::vector<uint8_t>> raws(raw_files.size());
  for (size_t f = 0; f < raw_files.size(); ++f) {
    if (!read_file(raw_files[f].c_str(), raws[f])) return 2;
  }

  const double spf = 1.0 / fps;
  const double start_time = static_cast<double>(start_frame) * spf;
  int handle = 0;
  int rc = xg_abc_open(abc_path, spf, start_time, animate ? 1 : 0,
                       sort_by_uv ? 1 : 0, &handle);
  if (rc != XG_OK) {
    std::fprintf(stderr, "xg_abc_open failed (%d): %s\n", rc, xg_last_error());
    return 2;
  }
  rc = xg_abc_write_first(handle, raws[0].data(), raws[0].size(), group,
                          guide ? 1 : 0, group_id);
  if (rc != XG_OK) {
    std::fprintf(stderr, "xg_abc_write_first failed (%d): %s\n", rc,
                 xg_last_error());
    xg_abc_close(handle);
    return 2;
  }
  for (size_t f = 1; f < raws.size(); ++f) {
    rc = xg_abc_write_frame(handle, raws[f].data(), raws[f].size(),
                            static_cast<double>(start_frame + f) * spf);
    if (rc != XG_OK) {
      std::fprintf(stderr, "xg_abc_write_frame failed (%d): %s\n", rc,
                   xg_last_error());
      xg_abc_close(handle);
      return 2;
    }
  }
  rc = xg_abc_close(handle);
  if (rc != XG_OK) {
    std::fprintf(stderr, "xg_abc_close failed (%d): %s\n", rc, xg_last_error());
    return 2;
  }
  std::printf("wrote %s (%s, %zu frame(s))\n", abc_path,
              animate ? "animated" : "static", raws.size());
  return 0;
}

int cmd_read(int argc, char** argv) {
  if (argc < 3) {
    print_usage();
    return 1;
  }
  try {
    Abc::IArchive archive(AbcCoreOgawa::ReadArchive(), argv[2]);
    Abc::IObject top = archive.getTop();
    std::printf("archive %s: %zu top-level object(s)\n", argv[2],
                top.getNumChildren());
    for (size_t i = 0; i < top.getNumChildren(); ++i) {
      Abc::IObject child = top.getChild(i);
      if (!AbcGeom::ICurves::matches(child.getHeader())) {
        std::printf("  object '%s': schema=%s (skipped)\n",
                    child.getName().c_str(),
                    child.getHeader().getMetaData().get("schema").c_str());
        continue;
      }
      AbcGeom::ICurves curves(child, Abc::kWrapExisting);
      AbcGeom::ICurvesSchema schema = curves.getSchema();
      const size_t num_samples = schema.getNumSamples();
      AbcGeom::ICurvesSchema::Sample sample;
      schema.get(sample, Abc::ISampleSelector(static_cast<AbcA::index_t>(0)));
      const Abc::Int32ArraySamplePtr nverts = sample.getCurvesNumVertices();
      const Abc::P3fArraySamplePtr positions = sample.getPositions();
      const Abc::UcharArraySamplePtr orders = sample.getOrders();
      const Abc::FloatArraySamplePtr knots = sample.getKnots();
      int64_t cv_sum = 0;
      if (nverts) {
        for (size_t k = 0; k < nverts->size(); ++k) cv_sum += (*nverts)[k];
      }
      AbcGeom::IFloatGeomParam widths_param = schema.getWidthsParam();
      const char* width_scope = "none";
      if (widths_param.valid()) {
        width_scope =
            widths_param.getScope() == AbcGeom::kVertexScope ? "vertex" : "other";
      }
      std::printf(
          "  curves '%s': samples=%zu num_curves=%zu total_cvs=%lld "
          "pos_cvs=%zu basis=%d wrap=%d type=%d knots=%zu widths=%s\n",
          child.getName().c_str(), num_samples,
          nverts ? nverts->size() : 0, static_cast<long long>(cv_sum),
          positions ? positions->size() : 0, static_cast<int>(sample.getBasis()),
          static_cast<int>(sample.getWrap()), static_cast<int>(sample.getType()),
          knots ? knots->size() : 0, width_scope);

      AbcA::TimeSamplingPtr ts = schema.getTimeSampling();
      if (ts) {
        std::printf("    time sampling: period=%.6g", 
                    ts->getTimeSamplingType().getTimePerCycle());
        const size_t print_count = num_samples < 6 ? num_samples : 6;
        for (size_t s = 0; s < print_count; ++s) {
          std::printf(" t[%zu]=%.6g", s,
                      ts->getSampleTime(static_cast<AbcA::index_t>(s)));
        }
        std::printf("\n");
      }

      Abc::ICompoundProperty arb = schema.getArbGeomParams();
      std::printf("    arb props:");
      for (size_t p = 0; p < arb.getNumProperties(); ++p) {
        std::printf(" %s", arb.getPropertyHeader(p).getName().c_str());
      }
      std::printf("\n");

      if (num_samples > 1) {
        bool consistent = true;
        for (size_t s = 0; s < num_samples; ++s) {
          AbcGeom::ICurvesSchema::Sample frame_sample;
          schema.get(frame_sample,
                     Abc::ISampleSelector(static_cast<AbcA::index_t>(s)));
          if (frame_sample.getPositions() &&
              frame_sample.getPositions()->size() != positions->size()) {
            consistent = false;
          }
        }
        std::printf("    per-frame CV count consistent across %zu samples: %s\n",
                    num_samples, consistent ? "yes" : "NO");
      }
    }
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "read failed: %s\n", e.what());
    return 2;
  }
}

int cmd_multi(int argc, char** argv) {
  if (argc < 5) {
    print_usage();
    return 1;
  }
  const char* abc_path = argv[2];
  std::vector<std::string> raw_files;
  int argi = 3;
  while (argi < argc && argv[argi][0] != '-') {
    raw_files.emplace_back(argv[argi]);
    ++argi;
  }
  if (raw_files.empty()) {
    print_usage();
    return 1;
  }
  std::vector<std::string> objects;
  bool animate = false;
  bool guide = false;
  bool sort_by_uv = false;
  int start_frame = 1;
  double fps = 24.0;
  for (int i = argi; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--object" && i + 1 < argc) {
      objects.emplace_back(argv[++i]);
    } else if (arg == "--animate") {
      animate = true;
    } else if (arg == "--sort") {
      sort_by_uv = true;
    } else if (arg == "--guide") {
      guide = true;
    } else if (arg == "--start" && i + 1 < argc) {
      start_frame = std::atoi(argv[++i]);
    } else if (arg == "--fps" && i + 1 < argc) {
      fps = std::atof(argv[++i]);
    } else {
      std::fprintf(stderr, "unknown option: %s\n", argv[i]);
      return 1;
    }
  }
  if (objects.empty()) {
    std::fprintf(stderr, "multi requires at least one --object <name>\n");
    return 1;
  }

  std::vector<std::vector<uint8_t>> raws(raw_files.size());
  for (size_t f = 0; f < raw_files.size(); ++f) {
    if (!read_file(raw_files[f].c_str(), raws[f])) return 2;
  }

  const double spf = 1.0 / fps;
  const double start_time = static_cast<double>(start_frame) * spf;
  int archive_handle = 0;
  int rc = xg_abc_open(abc_path, spf, start_time, animate ? 1 : 0,
                       sort_by_uv ? 1 : 0, &archive_handle);
  if (rc != XG_OK) {
    std::fprintf(stderr, "xg_abc_open failed (%d): %s\n", rc, xg_last_error());
    return 2;
  }
  std::vector<int> object_handles;
  for (const std::string& name : objects) {
    int obj = 0;
    const char* name_cstr = name.c_str();
    rc = xg_abc_add_curves(archive_handle, name_cstr, guide ? 1 : 0, -1, &obj);
    if (rc != XG_OK) {
      std::fprintf(stderr, "xg_abc_add_curves(%s) failed (%d): %s\n",
                   name.c_str(), rc, xg_last_error());
      xg_abc_close(archive_handle);
      return 2;
    }
    // Explicit objects own their group name; write it through the arb API so
    // xg_abc_write_first does not attempt a duplicate write.
    rc = xg_abc_write_arb_string(obj, "groom_group_name", &name_cstr, 1);
    if (rc != XG_OK) {
      std::fprintf(stderr, "xg_abc_write_arb_string(%s) failed (%d): %s\n",
                   name.c_str(), rc, xg_last_error());
      xg_abc_close(archive_handle);
      return 2;
    }
    object_handles.push_back(obj);
  }
  for (size_t f = 0; f < raws.size(); ++f) {
    for (size_t o = 0; o < object_handles.size(); ++o) {
      if (f == 0) {
        rc = xg_abc_write_first(object_handles[o], raws[f].data(),
                                raws[f].size(), objects[o].c_str(),
                                guide ? 1 : 0, -1);
      } else {
        rc = xg_abc_write_frame(
            object_handles[o], raws[f].data(), raws[f].size(),
            static_cast<double>(start_frame + f) * spf);
      }
      if (rc != XG_OK) {
        std::fprintf(stderr, "write object %s frame %zu failed (%d): %s\n",
                     objects[o].c_str(), f, rc, xg_last_error());
        xg_abc_close(archive_handle);
        return 2;
      }
    }
  }
  rc = xg_abc_close(archive_handle);
  if (rc != XG_OK) {
    std::fprintf(stderr, "xg_abc_close failed (%d): %s\n", rc, xg_last_error());
    return 2;
  }
  std::printf("wrote multi-object %s with %zu object(s) (%s, %zu frame(s))\n",
              abc_path, object_handles.size(), animate ? "animated" : "static",
              raws.size());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    print_usage();
    return 1;
  }
  const std::string cmd = argv[1];
  if (cmd == "version") {
    std::printf("xgcore 0.1.0\n");
    return 0;
  }
  if (cmd == "parse") return cmd_parse(argc, argv);
  if (cmd == "abc") return cmd_abc(argc, argv);
  if (cmd == "multi") return cmd_multi(argc, argv);
  if (cmd == "read") return cmd_read(argc, argv);
  print_usage();
  return 1;
}
