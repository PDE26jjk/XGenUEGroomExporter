## Context

See proposal.md - Why for motivation. Current state and constraints that shape the approach:

- The two Python exporters (`XGenUEGroomExporter.py`, `XGenDescriptionUEGroomExporter.py`, plus py2 twins) spend ~10 s per animation frame on a 400k-strand / 2M-CV groom: ~5 s in `getXgenData` (block parsing, zlib inflate, record unpacking) and ~5 s in strand sorting plus per-element imath array filling.
- The raw input format was verified against a 32.4 MB reference sample: 15 blocks (1 JSON header + 14 deflated groups), 399,874 curves, 1,999,370 CVs, ~99 MB decompressed per frame.
- Constraints: all Maya versions including Python 2.7 must remain supported; numpy is not acceptable; the user chose a ctypes-callable DLL over pybind11; the C++ project must live in a new `cpp/` folder and must not modify the existing Python scripts; the Maya-side `writeBinary()` cost (~2.7 s/frame) is deferred.

## Goals / Non-Goals

**Goals:**

- A standalone `cpp/` CMake project (x64, MSVC 2022) that builds one self-contained `xgcore.dll` with a pure C ABI.
- Parser that reproduces the exact byte-level semantics of `getXgenData` (blocks, JSON header, per-group inflate, inner blocks, item payloads).
- UV strand ordering with the same semantics as the Python `get_index2order`.
- Direct Alembic `.abc` writing via the Alembic C++ API matching the current Python output semantics.
- A CLI test tool and an A/B verification harness against the reference sample and the Python exporter.

**Non-Goals:**

- Modifying the existing 4 Python scripts (integration is a follow-up change).
- Optimizing `writeBinary()` or `ConvertToInteractive` (Maya-side costs).
- UV baking in C++ (depends on Maya `getUVAtPoint`).
- Guide-id / Ptex logic (stays Python-side).
- Fixing the per-frame vertex-count mismatch known issue (resampling).
- macOS/Linux builds; pybind11; numpy.

## Decisions

### 1. Pure C ABI DLL + ctypes, not pybind11

One x64 DLL with unmangled C exports works identically from Python 2.7 (Maya 2018/2019) and all Python 3.x Maya versions, because it depends on no Python ABI. ctypes releases the GIL during calls, so the DLL can parallelize inflate/sort internally.

Alternatives considered: pybind11 (rejected: no Python 2.7 support, one `.pyd` per Maya Python version, compiler-runtime matching per version) and a native Maya plugin `.mll` (rejected: per-Maya-version binaries and devkit matching, plugin lifecycle complexity).

### 2. Fully static, self-contained DLL

Build Alembic 1.8.12, Imath 3.1, and zlib as static libraries with `/MT` and link them into `xgcore.dll`. The DLL then has no runtime dependencies beyond the OS.

Alternatives considered: linking vcpkg's prebuilt shared Alembic (rejected: drags `Alembic.dll`, `Imath-3_1.dll`, zlib DLLs to every user) and the existing `alembic-master` Debug build (rejected: Debug, shared, not redistributable as-is).

### 3. Dependency acquisition via CMake overrides with FetchContent fallback

The project supports `ALEMBIC_SOURCE_DIR`, `IMATH_SOURCE_DIR`, and `ZLIB_SOURCE_DIR` cache variables pointing at local checkouts (the machine already has `R:\git\alembic` at 1.8.12 and vcpkg-installed Imath/zlib), falling back to CMake FetchContent when unset. This keeps the build working offline and reproducible.

### 4. Two-stage API: parser first, then Alembic writer

The C ABI is split so each stage is independently verifiable:

```c
// Stage 1 - parse
int xg_parse_frame_size(const uint8_t* raw, size_t n, int sort_by_uv, int64_t* out);
int xg_parse_frame(const uint8_t* raw, size_t n, int sort_by_uv,
                   float* positions, int32_t* curve_offsets, float* widths,
                   float* root_points, int32_t* index2order);

// Stage 2 - Alembic writer
int xg_abc_open(const char* path, double spf, double start_time, int animate, int* h);
int xg_abc_write_first(int h, const uint8_t* raw, size_t n, const char* group_name,
                       int is_guide, int group_id);
int xg_abc_write_frame(int h, const uint8_t* raw, size_t n, double time);
int xg_abc_write_root_uv(int h, const float* uv, size_t n);
int xg_abc_close(int h);
const char* xg_last_error(void);
```

The size-query + fill pattern keeps caller-owned, reusable buffers and avoids allocations crossing the C boundary. The archive handle (`int h`) wraps a small state object in the DLL; a single concurrent export is the expected use.

Rationale: the parser is the highest-risk part (binary format), so it gets its own CLI testable entry point before any Alembic code exists.

### 5. Exact byte-level parse semantics

Replicate the verified format:

- Block header: `u32 type_code @ 0`, `u64 size @ 8`; block payload `[start+16, start+16+size)`.
- Group block: 16-byte block header + 32-byte sub-header; inflate from `start+48` to EOF (zlib ignores trailing bytes), mirroring the Python `zlib.decompress(rawData[block+32:])` behavior. Non-deflated groups use the block range directly.
- Inner blocks inside each decompressed group use the same 16-byte header scheme; item payloads are addressed by the header JSON's (group, index) references.
- Item encodings: `PrimitiveInfos` = little-endian `u32 offset, u64 length` records; `Positions`/`WIDTH_CV`/`FaceUV` = float32 arrays; `FaceId` = int32 array.
- The JSON header (3.6 KB) is parsed in C++ with a vendored single-header JSON parser so the DLL stays self-contained and testable without Python.

### 6. Strand ordering semantics

The permutation must match Python's `sorted((key, i) for i, key in enumerate(order_list))` with `key = (faceId, u, v)`. C++ uses a small POD `{int32_t face_id; float u; float v; uint32_t index;}` sorted by `std::tie(face_id, u, v, index)`. For well-formed XGen UV data (no NaN) this is exactly equivalent. The output `index2order` maps each original strand index to its sorted position, as in Python.

### 7. Parallel inflate

Groups are independent, so inflate runs on a small `std::async` pool (one task per group). Each task owns its own `z_stream`; no shared state. ctypes releases the GIL, so this parallelizes freely. Expected: 99 MB inflate drops from ~0.32 s to ~0.1 s on typical multi-core machines.

### 8. Alembic writing structure

- `Abc::OArchive` (Ogawa) with a `TimeSampling(spf, spf * start_time)` when animated.
- One `AbcGeom::OCurves` per groom; first sample writes basis/wrap/type, per-curve orders/knots/vertex counts, vertex-scope widths, and arbitrary geometry parameters (`groom_group_name` OStringArrayProperty, `groom_guide` OInt16ArrayProperty, `groom_group_id` OInt32ArrayProperty) via `schema.getArbGeomParams()`.
- Subsequent frames write position-only samples; `groom_root_uv` (OV2fArrayProperty) is written after all frames via `xg_abc_write_root_uv`.

### 9. Verification strategy

- A Python reference script (existing `getXgenData`/`get_index2order` logic) dumps expected counts, permutation, and positions from the reference `raw_data.bin`.
- The CLI tool prints the same outputs; an A/B harness compares them exactly.
- The generated `.abc` is read back with Alembic (`AbcOpen`/`AbcGeom`) and, at the end, imported into UE as a smoke test.
- Compatibility target is UE-importable structure and properties, not byte-identical files.

## Risks / Trade-offs

- [The ~2.7 s/frame `writeBinary()` cost remains and becomes the floor for the follow-up integration] -> Explicitly out of scope; the parser/writer deliverables are still measured and reported; a separate investigation targets the Maya-side read path.
- [Alembic 1.8.12 output differs byte-wise from Maya's bundled Alembic] -> Verified by read-back and UE import rather than byte comparison; property names and scopes are kept identical.
- [Float tie-break edge cases in sorting (e.g., NaN) could diverge from Python] -> Not expected from XGen UV data; the A/B harness covers the reference sample; comparator mirrors Python semantics for non-NaN floats.
- [Offline build: FetchContent requires network] -> Local source overrides (`*_SOURCE_DIR`) are the primary path; document them in the build README.
- [Static CRT `/MT` DLL loaded into Maya] -> Safe because the interface is pure C and caller-owned buffers; no CRT objects cross the boundary.
- [Binary format assumptions break on unseen XGen versions] -> The parser validates block/header invariants and reports clear errors instead of misparsing.

## Migration Plan

No production migration: the existing Python scripts keep working unchanged. This change lands a standalone `cpp/` project with its own build, tests, and CLI tool. A follow-up change wires the DLL into the scripts behind a `try/import` fallback and measures end-to-end gains.

## Open Questions

- Whether macOS/Linux builds are ever needed: deferred; Windows x64 is the only target for this change.
- Whether `GroupBase64` groups should be supported instead of erroring: kept as an error to match current Python behavior; adding support later is a small spec change.

