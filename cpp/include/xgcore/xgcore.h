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
  int64_t total_records; /* all strand records, including length < 2 */
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
 * Parses one frame and exposes per-record FaceId/FaceUV plus the strand
 * ordering used for guide-id-from-ptex support. Call xg_parse_frame_size
 * first and allocate:
 *   face_ids   count * int32   (raw record order)
 *   face_uvs   count * 2 float (raw record order)
 *   order      count * int32   (raw record index -> sorted index; identity
 *                               when sort_by_uv == 0)
 * On success fills *out_count with the number of records (>= num_curves when
 * short strands are present) and returns XG_OK.
 */
XGCORE_API int xg_parse_face(const uint8_t* raw, size_t raw_len, int sort_by_uv,
                             int32_t* face_ids, float* face_uvs,
                             int32_t* order, int64_t* out_count);

/*
 * Alembic writer.
 *
 * xg_abc_open creates/truncates the .abc file. If animate != 0 the archive
 * uses a uniform time sampling starting at start_time with period spf
 * (seconds); each xg_abc_write_frame call appends the next sample.
 * sort_by_uv != 0 orders strands by (faceId, u, v) before writing.
 *
 * xg_abc_add_curves adds a top-level curve object and returns a per-object
 * handle. It writes groom_guide (when is_guide != 0) and groom_group_id only
 * when group_id >= 0 (pass -1 to write it later through
 * xg_abc_write_arb_int32, e.g. after a group-id pass). groom_group_name is
 * written by the caller (typically xg_abc_write_arb_string), so the object
 * name and the group name can differ, as in the Python exporters.
 *
 * xg_abc_write_first writes the static curve topology (basis, orders, knots,
 * vertex counts, vertex widths) and the first frame's positions from raw XGen
 * bytes. It accepts either an object handle from xg_abc_add_curves or an
 * archive handle; on an archive handle a single implicit object named by
 * group_name is created lazily and groom_group_name is written from group_name
 * (backward-compatible single-object flow). On an explicit object handle the
 * group name is never written here, so the caller's write_group_name cannot
 * collide with this path.
 * xg_abc_write_curves_first is the array-input variant for Maya curves and
 * guides: positions (num_cvs * 3 floats), per-curve nverts/orders, knots, and
 * optional per-CV widths (pass NULL with a 0 count to skip widths).
 * xg_abc_write_root_uv stores the per-strand root UVs (constant scope) and is
 * typically called once after all frames. xg_abc_write_arb_* write generic
 * arbitrary parameters (array properties under arbGeomParams).
 * xg_abc_close finalizes the archive and invalidates all of its object
 * handles.
 */
XGCORE_API int xg_abc_open(const char* path, double spf, double start_time,
                           int animate, int sort_by_uv, int* out_handle);

XGCORE_API int xg_abc_add_curves(int archive_handle, const char* name,
                                 int is_guide, int group_id,
                                 int* out_object_handle);

XGCORE_API int xg_abc_write_first(int handle, const uint8_t* raw, size_t raw_len,
                                  const char* group_name, int is_guide,
                                  int group_id);

XGCORE_API int xg_abc_write_frame(int handle, const uint8_t* raw, size_t raw_len,
                                  double time);

XGCORE_API int xg_abc_write_curves_first(int object_handle,
                                         const float* positions,
                                         const int32_t* nverts,
                                         const int32_t* orders,
                                         const float* knots,
                                         const float* widths,
                                         int64_t num_curves, int64_t num_cvs,
                                         int64_t knots_count);

XGCORE_API int xg_abc_write_curves_frame(int object_handle,
                                         const float* positions,
                                         int64_t num_cvs);

XGCORE_API int xg_abc_write_arb_string(int object_handle, const char* name,
                                       const char* const* values,
                                       int64_t count);

XGCORE_API int xg_abc_write_arb_int32(int object_handle, const char* name,
                                      const int32_t* values, int64_t count);

XGCORE_API int xg_abc_write_arb_float(int object_handle, const char* name,
                                      const float* values, int64_t count);

XGCORE_API int xg_abc_write_arb_v2f(int object_handle, const char* name,
                                    const float* values, int64_t count);

XGCORE_API int xg_abc_write_root_uv(int handle, const float* uv, size_t count);

XGCORE_API int xg_abc_close(int handle);

#ifdef __cplusplus
}
#endif

#endif /* XGCORE_XGCORE_H */
