## Why

The pure-Python XGen groom exporter spends roughly 10 seconds per animation frame on a typical groom: about 5 seconds decoding `outSplineData` (block parsing, zlib inflate, record unpacking) and another 5 seconds sorting ~400k strands by (faceId, u, v) and filling imath arrays element-by-element. A 100 MB animation export takes ~77 seconds. Moving this compute into a compiled core removes the Python-loop overhead entirely and lets the Alembic file be written directly from C++.

## What Changes

- Add a new `cpp/` folder with a standalone C++ project (CMake, x64, MSVC 2022) that is fully independent of the existing Python scripts:
  - Parse the XGen `outSplineData` binary format: block table, JSON header, per-group zlib inflate, inner blocks, and item payloads (`PrimitiveInfos`, `Positions`, `WIDTH_CV`, `FaceId`, `FaceUV`).
  - Reorder strands by `(faceId, u, v)` with identical semantics to the current Python `get_index2order`.
  - Write Alembic `.abc` files directly through the Alembic C++ API: `OCurves` with cubic B-spline basis, knots/orders/widths, arbitrary geometry parameters (`groom_group_name`, `groom_guide`, `groom_group_id`, `groom_root_uv`), and time sampling for animation, matching the semantics of the current Python output.
  - Expose a pure C ABI DLL (`xgcore.dll`) callable via ctypes from both Python 2.7 (Maya 2018/2019) and Python 3.x (Maya 2020+).
- Provide a standalone CLI test tool that converts a sample `raw_data.bin` into `.abc` for A/B verification against the Python exporter.
- **No changes to the existing 4 Python scripts in this change.** Python integration (thin ctypes wrapper with pure-Python fallback) is a follow-up change.

## Capabilities

### New Capabilities

- `xg-core`: compiled C++ core that parses XGen spline data binary and writes UE-compatible Alembic groom files.

### Modified Capabilities

- None. No existing specs or requirement changes.

## Impact

- New folder: `cpp/` (CMake project, source, tests, build docs). Existing scripts, `openspec/`, and `.git` are untouched.
- Build dependencies (all statically linked, `/MT`, x64): Alembic 1.8.12, Imath 3.1, zlib. Local checkouts (`R:\git\alembic`) are usable via a CMake override; CMake FetchContent is the fallback.
- Deliverables: `xgcore.dll` (C ABI), `xgcore_test` CLI, unit/integration tests, and an A/B comparison harness.
- Performance target for the follow-up integration: per-frame export time drops from ~10 s to ~3.5–5 s on the profiled 100 MB animation. The Maya-side `writeBinary()` cost (~2.7 s/frame) is explicitly out of scope for this change; it will be revisited separately.

