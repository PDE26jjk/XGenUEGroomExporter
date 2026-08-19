## 1. Project Scaffold and Dependencies

- [x] 1.1 Create the `cpp/` CMake project skeleton: `CMakeLists.txt`, `src/`, `include/`, `tests/`, `tools/`, `README.md`, and a `.gitignore` for build directories
- [x] 1.2 Wire dependency acquisition: `ALEMBIC_SOURCE_DIR`, `IMATH_SOURCE_DIR`, `ZLIB_SOURCE_DIR` cache overrides with FetchContent fallback (Alembic 1.8.12, Imath 3.1, zlib), all static, `/MT`, x64
- [x] 1.3 Add a NOTICE/THIRD-PARTY file with Alembic (BSD-3), Imath (BSD-3), and zlib license texts
- [x] 1.4 Verify the build produces `xgcore.dll` with no external DLL dependencies (`dumpbin /dependents` clean)

## 2. Parser Core

- [x] 2.1 Implement the block-table parser and JSON header parsing (vendored single-header JSON), including the `Items`/`RefMeshArray` (group, index) reference mapping
- [x] 2.2 Implement group payload extraction: deflated path (inflate from `start+48` to EOF, ignoring trailing bytes) and non-deflated path; inner block slicing
- [x] 2.3 Implement item decoders: `PrimitiveInfos` (little-endian u32/u64 records), `Positions`/`WIDTH_CV`/`FaceUV` (float32 arrays), `FaceId` (int32 array); `GroupBase64=true` returns a clear error
- [x] 2.4 Implement the strand permutation ordered by `(faceId, u, v, index)` with semantics identical to the Python reference
- [x] 2.5 Implement the C ABI entry points `xg_parse_frame_size` / `xg_parse_frame`, integer status codes, and `xg_last_error`

## 3. Parser Verification

- [x] 3.1 Add a Python reference dumper (reusing the existing `getXgenData`/`get_index2order` logic) that emits expected counts, permutation, positions, and widths for the reference sample
- [x] 3.2 Add the `xgcore_test` CLI tool that prints parse results from a raw data file
- [x] 3.3 Add an A/B harness comparing DLL output exactly against the Python reference: curve count, CV count, permutation, positions, widths
- [x] 3.4 Add unit tests for block parsing, deflated/non-deflated paths, base64 error, and malformed-input error handling

## 4. Alembic Writer

- [x] 4.1 Implement `xg_abc_open` (OArchive + optional TimeSampling for animation)
- [x] 4.2 Implement `xg_abc_write_first`: OCurves static properties (cubic B-spline basis, non-periodic wrap, orders/knots/vertex counts), vertex-scope widths, and arbitrary geometry parameters `groom_group_name`, `groom_guide`, `groom_group_id`
- [x] 4.3 Implement `xg_abc_write_frame`: position-only samples per frame
- [x] 4.4 Implement `xg_abc_write_root_uv` and `xg_abc_close`
- [x] 4.5 Wire the writer into the CLI: reference raw data -> `.abc` for static and animated modes

## 5. Output Verification

- [x] 5.1 Read back the generated `.abc` with Alembic and compare curve counts, per-curve vertex counts, widths, and arbitrary parameters against the Python exporter output
- [x] 5.2 Prepare static and animated `.abc` samples plus a UE import checklist for the developer; the developer performs the actual UE import (the agent does not run UE)
- [x] 5.3 Measure and record parse+write time on the reference sample in the README

## 6. Documentation and Handoff

- [x] 6.1 Write the build README: prerequisites, local dependency overrides, build commands, and output artifacts
- [x] 6.2 Document the C ABI in the public header with a usage example for the future Python wrapper
- [x] 6.3 Final review: `openspec validate` passes and the existing Python scripts are confirmed untouched
