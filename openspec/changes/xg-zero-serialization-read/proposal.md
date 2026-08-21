## Why

The C++-accelerated export path still spends ~2.9 s/frame on
`MFnPluginData.writeBinary()` (XGen's full spline serialization) plus another
~1.1 s on DLL-side zlib decompress + parse, and ~0.65 s/frame on
`ConvertToInteractive`. Reverse engineering of `AdskXGen.dll` /
`xgenToolkit.mll` (Maya 2019 and 2025) shows the live `XgSplineData` is
directly reachable at `MPxData + 0x10` with stable, version-identical memory
layouts and exported member functions. The serialization round-trip can be
skipped entirely by reading the in-memory arrays, which is the remaining
dominant per-frame cost. Measured baseline at density 10: pure Python
14.71 s, C++ accelerated 4.57 s for a static frame; animation frames are
dominated by `writeBinary` + parse per frame.

## What Changes

- Add zero-serialization read entry points to `xgcore.dll` (no OpenMaya
  dependency): given the live `MPxData` address, walk `XgSplineData` /
  `XgSplineDataItem` / `XgMappable` in memory and feed the existing Alembic
  writer the same stream arrays the current parser produces. One DLL serves
  all Maya versions; no per-version Maya plugin (`.mll`) is introduced.
- Extend the embedded wrapper in the four exporter scripts: acquire the
  `MPxData*` address from `MFnPluginData.data()` (spike-gated; the current
  `writeBinary()` path remains the default and the fallback), probe the
  direct-read route at export start, and route `CppXGenProxyEveryFrame` /
  `CppCurvesProxy` through the new entry points.
- Improve the DLL debug loop: explicit DLL override (env var or sidecar file,
  absolute paths allowed), lazy per-export reload, and versioned dev DLL
  filenames so a rebuilt DLL can be picked up without restarting Maya.
- Formalize the per-frame root-stability verification (same-index strand
  roots unchanged across animation frames) as a regression check that both
  export paths must pass.
- **BREAKING**: none. When the direct-read route is unavailable or fails any
  layout/ABI probe, the accelerated export runs the current
  `writeBinary()`+parse path; all existing behavior, options, and parity
  guarantees are unchanged.

## Capabilities

### New Capabilities

- `xg-zero-serialization`: optional direct-memory read of live XGen spline
  data inside the accelerated export, with automatic fallback to the
  serialized path.
- `xg-dll-discovery`: DLL location and load semantics for the embedded
  wrapper, including explicit override and per-export reload in development.

### Modified Capabilities

- None. The existing `cpp-accelerated-export` requirements (opt-in
  acceleration, feature parity, single writer, clear failure reporting)
  remain unchanged; this change only replaces the accelerated path's data
  source while preserving those behaviors.

## Impact

- `cpp/` (`include/xgcore/xgcore.h`, `src/xgcore_api.cpp`, new reader module):
  new memory-read entry points; the Alembic writer and existing entry points
  stay stable.
- The four Python exporter scripts (`XGenDescriptionUEGroomExporter.py`,
  `XGenUEGroomExporter.py` and `_py2` twins): embedded wrapper gains pointer
  acquisition, the new entry points, lazy load, and override semantics; the
  C++ proxies switch data source.
- New dev build script for versioned DLL output; `docs/maya-agent-debugging.md`
  and `cpp/README.md` gain the dev-loop recipe and zero-serialization notes.
- Verification tooling: read-back root-stability analyzer added under the
  existing verification workflow.
- No new third-party Python dependencies; `xgcore.dll` remains the only
  additional runtime artifact.
