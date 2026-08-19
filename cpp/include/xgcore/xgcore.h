#ifndef XGCORE_XGCORE_H
#define XGCORE_XGCORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#  ifdef XGCORE_BUILDING_DLL
#    define XGCORE_API __declspec(dllexport)
#  else
#    define XGCORE_API __declspec(dllimport)
#  endif
#else
#  define XGCORE_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Status codes returned by all xgcore functions. */
#define XG_OK              0  /* success */
#define XG_ERR_INVALID_ARG 1  /* null pointer, empty input, bad handle */
#define XG_ERR_MALFORMED   2  /* input bytes are not a valid XgSplineData stream */
#define XG_ERR_UNSUPPORTED 3  /* valid stream using a feature xgcore cannot handle */
#define XG_ERR_IO          4  /* file read/write failure */
#define XG_ERR_ALEMBIC     5  /* Alembic writer failure */
#define XG_ERR_STATE       6  /* call sequence violation (e.g. frame before open) */

/* Parsed frame summary. */
typedef struct XgParseInfo {
  int64_t num_curves; /* strands with length >= 2 */
  int64_t num_cvs;    /* total CVs of those strands */
} XgParseInfo;

/*
 * Returns the message for the most recent failure on the calling thread.
 * Valid until the next xgcore call on the same thread. Empty string on success.
 */
XGCORE_API const char* xg_last_error(void);

/*
 * Parses one frame of raw XGen spline data (the bytes returned by
 * MFnPluginData.writeBinary on outSplineData).
 *
 * sort_by_uv != 0 reorders strands lexicographically by (faceId, u, v),
 * matching the Python exporter's get_index2order semantics. Output buffers
 * are then written in sorted order; otherwise raw order is preserved.
 *
 * On success fills *info and returns XG_OK. Call xg_parse_frame_size first to
 * obtain num_curves/num_cvs, then allocate:
 *   positions      num_cvs * 3 floats
 *   curve_offsets  (num_curves + 1) int32, CV start index of each strand
 *   widths         num_cvs floats
 *   root_points    num_curves * 3 floats (first CV of each strand)
 *   index2order    num_curves int32 (original index -> output index)
 */
XGCORE_API int xg_parse_frame_size(const uint8_t* raw, size_t raw_len,
                                   int sort_by_uv, XgParseInfo* info);

XGCORE_API int xg_parse_frame(const uint8_t* raw, size_t raw_len, int sort_by_uv,
                              float* positions, int32_t* curve_offsets,
                              float* widths, float* root_points,
                              int32_t* index2order, XgParseInfo* info);

/*
 * Alembic writer.
 *
 * xg_abc_open creates/truncates the .abc file. If animate != 0 the archive
 * uses a uniform time sampling starting at start_time with period spf
 * (seconds); each xg_abc_write_frame call appends the next sample.
 * sort_by_uv != 0 orders strands by (faceId, u, v) before writing.
 * xg_abc_write_first writes the static curve topology (basis, orders, knots,
 * vertex counts, vertex widths) plus arbitrary parameters groom_group_name,
 * groom_guide, groom_group_id, and the first frame's positions.
 * xg_abc_write_root_uv stores the per-strand root UVs (constant scope) and is
 * typically called once after all frames. xg_abc_close finalizes the archive.
 */
XGCORE_API int xg_abc_open(const char* path, double spf, double start_time,
                           int animate, int sort_by_uv, int* out_handle);

XGCORE_API int xg_abc_write_first(int handle, const uint8_t* raw, size_t raw_len,
                                  const char* group_name, int is_guide,
                                  int group_id);

XGCORE_API int xg_abc_write_frame(int handle, const uint8_t* raw, size_t raw_len,
                                  double time);

XGCORE_API int xg_abc_write_root_uv(int handle, const float* uv, size_t count);

XGCORE_API int xg_abc_close(int handle);

#ifdef __cplusplus
}
#endif

#endif /* XGCORE_XGCORE_H */
