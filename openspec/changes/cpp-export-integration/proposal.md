## Why

The compiled export core (`cpp/`, `xgcore.dll`) is built and verified: it parses
XGen spline data and writes Alembic groom files ~8x faster than the pure-Python
path on the reference sample. The exporters currently do not use it. This
change wires the core into the four Python exporter scripts as an optional,
opt-in acceleration path while keeping the original Python export logic fully
intact for maximum compatibility.

## What Changes

- Add a shared Python wrapper module `xgcore_wrapper.py` (Python 2.7 and 3.x
  compatible) that loads `xgcore.dll` through ctypes and exposes `CppArchive`
  plus `CppXGenProxy` / `CppCurvesProxy` / `CppGuideProxy` classes whose
  interfaces mirror the existing proxy classes.
- Add a "Use C++ acceleration" checkbox to both exporter UIs
  (`XGenUEGroomExporter.py`, `XGenDescriptionUEGroomExporter.py`). The checkbox
  is off by default and only enabled when the DLL loads successfully.
- Branch the `save_abc` methods: when acceleration is enabled, the export runs
  through the C++ core (single writer per export); otherwise the original
  Python code path runs unchanged.
- Extend the xgcore C ABI to support the Python exporters:
  - multi-object archives (`xg_abc_add_curves` returning a per-object handle);
  - array-input curve writing for Maya curves and guides
    (`xg_abc_write_curves_first` / `xg_abc_write_curves_frame`);
  - generic arbitrary-parameter writes (string / int32 / float / v2f);
  - parse output extension exposing `FaceId`, `FaceUV`, and the strand order
    for guide-id-from-ptex support.
- Preserve every existing UI option in the accelerated path (bake UV, guides,
  group ids, card ids, ptex guide ids, animation, spline animation), verified
  by dual-path A/B regression.

**BREAKING**: none. The original Python export behavior is unchanged when the
checkbox is off or the DLL is unavailable.

## Capabilities

### New Capabilities

- `cpp-accelerated-export`: optional C++-accelerated grooming export from the
  Python exporters, selectable per export, with the original Python path kept
  as the default and fallback.

### Modified Capabilities

- None.

## Impact

- Affected files: `XGenUEGroomExporter.py`, `XGenDescriptionUEGroomExporter.py`
  and their `_py2` twins gain a UI checkbox and a `save_abc` branch; a new
  `xgcore_wrapper.py` is added next to the scripts.
- xgcore C ABI (`cpp/include/xgcore/xgcore.h`) gains new entry points; existing
  entry points stay stable.
- The compiled `xgcore.dll` becomes a distributable artifact next to the
  scripts; the pure-Python path remains fully functional without it.
- No new third-party Python dependencies; the wrapper uses ctypes only.

