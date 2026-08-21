#ifndef XGCORE_ABC_WRITER_H
#define XGCORE_ABC_WRITER_H

#include <cstddef>
#include <cstdint>

namespace xgcore {

// Opaque Alembic archive / object handles; values start at 1 (0 = invalid).
// An archive owns an OArchive; object handles address top-level OCurves
// children created with abc_add_curves (or the lazy implicit single object).
struct AbcArchive;
struct AbcObject;

int abc_open(const char* path, double spf, double start_time, bool animate,
             bool sort_by_uv, int& out_handle);
int abc_add_curves(int archive_handle, const char* name, bool is_guide,
                   int group_id, int& out_object_handle);
int abc_write_first(int handle, const uint8_t* raw, size_t raw_len,
                    const char* group_name, bool is_guide, int group_id);
int abc_write_frame(int handle, const uint8_t* raw, size_t raw_len,
                    double time);
int abc_write_curves_first(int object_handle, const float* positions,
                           const int32_t* nverts, const int32_t* orders,
                           const float* knots, const float* widths,
                           int64_t num_curves, int64_t num_cvs,
                           int64_t knots_count);
int abc_write_curves_frame(int object_handle, const float* positions,
                           int64_t num_cvs);
int abc_write_arb_string(int object_handle, const char* name,
                         const char* const* values, int64_t count);
int abc_write_arb_int32(int object_handle, const char* name,
                        const int32_t* values, int64_t count);
int abc_write_arb_float(int object_handle, const char* name,
                        const float* values, int64_t count);
int abc_write_arb_v2f(int object_handle, const char* name,
                      const float* values, int64_t count);
int abc_write_root_uv(int handle, const float* uv, size_t count);
int abc_close(int handle);

}  // namespace xgcore

#endif  // XGCORE_ABC_WRITER_H
