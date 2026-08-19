#ifndef XGCORE_ABC_WRITER_H
#define XGCORE_ABC_WRITER_H

#include <cstddef>
#include <cstdint>

namespace xgcore {

// Opaque Alembic archive handle; values start at 1 (0 = invalid).
struct AbcWriter;

int abc_open(const char* path, double spf, double start_time, bool animate,
             bool sort_by_uv, int& out_handle);
int abc_write_first(int handle, const uint8_t* raw, size_t raw_len,
                    const char* group_name, bool is_guide, int group_id);
int abc_write_frame(int handle, const uint8_t* raw, size_t raw_len,
                    double time);
int abc_write_root_uv(int handle, const float* uv, size_t count);
int abc_close(int handle);

}  // namespace xgcore

#endif  // XGCORE_ABC_WRITER_H
