## Context

See proposal.md - Why. Current accelerated flow per frame:

```text
ConvertToInteractive (~0.65 s)
  -> outSplineData plug -> MFnPluginData -> writeBinary() (~2.9 s)
  -> xgcore.dll zlib decompress + JSON header + block parse (~1.1 s)
  -> UV sort -> Alembic writer
```

Measured at density 10 on Maya 2025 (`hair_debug.mb`): pure Python 14.71 s,
C++ accelerated 4.57 s for a static frame; a 2-frame spline-animation export
takes 11.98 s and produces 2 samples whose per-frame geometry differs because
the scene's guide 3 tip is genuinely animated (animCurves on
`xgGuide3Shape.controlPoints_3`), not because of ordering. The remaining
dominant per-frame cost is `writeBinary()` plus the DLL-side parse.

Reverse-engineering facts this design relies on (see the developer's IDA
report; verified identical on Maya 2019 and 2025):

- `outSplineData`'s `MPxData` subclass holds `std::shared_ptr<XgSplineData>`
  at `+0x10`; the object pointer is a plain member read.
- `XgSplineData` items live in a `multimap<string, shared_ptr<XgSplineDataItem>>`
  at `+0x10`; `itemCount` / `getItemByGenIndex` / `itemBegin` / `itemEnd` are
  exported symbols in both versions.
- `XgSplineDataItem` holds a `map<int /*XG_STREAM_SEMANTIC*/, XgMappableBase*>`
  at `+0x00`. Stream codes: 1 = primitiveInfos (uint32 offset/len pairs),
  4 = positions (float3), 16 = texcoords (float2), 0x2000 = faceUV (float2),
  0x4000 = faceId (uint32), 0x20000 = widthPerCV (float).
- `XgMappable<T>` at `+0x08` holds a `shared_ptr<XgVectorMappableImpl<T>>`;
  the impl has plain `begin`/`end` pointers at `+0x08`/`+0x10`, so element
  count = `(end - begin) / sizeof(T)`.
- Member functions `positions()` / `widthPerCV()` / `faceUV()` / `faceId()` /
  `primitiveInfos()` are exported in both versions with the same symbol set;
  function addresses differ per version, so symbols are resolved by name at
  runtime, never by address.

Constraints from the developer:

- No per-version Maya plugin (`.mll`); one `xgcore.dll` must serve all Maya
  versions (it currently has no OpenMaya dependency).
- `ConvertToInteractive` stays per frame: the converted interactive node is a
  snapshot of the classic description and does not track its animation.
- The pain point is speed; sorting is already verified correct (roots are
  stable across frames; per-frame geometry differences are scene animation).
- Development iteration must not require restarting Maya (DLL file lock).

## Goals / Non-Goals

**Goals:**

- Remove `writeBinary()` and the raw-binary parse from the accelerated export
  by reading the live in-memory `XgSplineData` arrays, without changing
  output parity or the single-writer rule.
- Keep the reader inside `xgcore.dll` (no OpenMaya dependency, no `.mll`).
- Provide a dev loop that picks up rebuilt DLLs without a Maya restart.
- Formalize the root-stability check as a regression verification.

**Non-Goals:**

- Removing or optimizing `ConvertToInteractive` (snapshot conversion stays;
  the C-2 direct-generation route is a separate future change).
- Stabilizing per-frame generation (sort-key rounding / resampling / fixed
  seed) — deferred; today's UV sort is verified correct for the test scene.
- Changing the pure-Python export path or the serialized accelerated path's
  behavior.
- Supporting Maya versions other than 2019/2025, or non-Windows builds.

## Decisions

### 1. Reader lives in xgcore.dll; Python supplies the `MPxData` address

The wrapper obtains the `MPxData*` address from `MFnPluginData.data()` (see
decision 2) and passes it as an integer to new C ABI entry points:

```cpp
// light probe: layout + exported-symbol check
int xg_read_xgen_probe(uint64_t pdata_addr, int* out_ok);
// first frame / subsequent frames: read memory and write into the archive
int xg_abc_write_xgen_memory_first(int handle, uint64_t pdata_addr, ...);
int xg_abc_write_xgen_memory_frame(int handle, uint64_t pdata_addr, ...);
```

Inside the DLL: read `XgSplineData*` at `pdata_addr + 0x10`; enumerate items
preferring exported `itemCount` + `getItemByGenIndex` (resolved via
`GetProcAddress` with the version-stable mangled names); read each stream
preferring exported `positions()` / `widthPerCV()` / `faceUV()` / `faceId()` /
`primitiveInfos()`, taking the returned `XgMappable&` and dereferencing
`impl + 0x08/+0x10`. The reader fills the exact same flat arrays the current
parser produces (positions, widths, face ids/uvs, primitive records, strand
order), then hands them to the existing sort + writer path unchanged.

Alternatives considered: a per-version `.mll` reader (rejected by the
developer: too much distribution/ABI burden); a Python-side ctypes reader
that walks the layouts directly (kept as the fallback discussion in decision
2 — the pointer acquisition is Python-side anyway, but stream decoding stays
in C++ to avoid reimplementing the MSVC tree layouts in Python); writing the
Alembic from a new plugin (rejected: duplicates the verified writer and
violates the single-DLL distribution model).

### 2. Pointer acquisition is spike-gated (Python side)

Task 1 of this change is a spike that answers: can Python read the raw
`MPxData*` address? Candidates, in order:

1. `MFnPluginData.data()` returns a SWIG-wrapped `MPxData`; probe `.this`
   (and the `PySwigObject` pointer forms) for the object address.
2. If `.this` is unavailable, inspect what the returned proxy exposes and
   whether `MObject` handles can be dereferenced safely.
3. If neither works, the zero-serialization scope is blocked and the change
   reduces to the dev loop + verification harness (recorded as a decision
   gate in tasks.md; no spec change needed because the fallback behavior is
   already specced).

The spike runs on the already-open Maya 2025 instance via the agent bridge;
the result is recorded in `docs/maya-agent-debugging.md`.

### 3. Probe + fallback semantics

Before any archive is opened, the wrapper calls `xg_read_xgen_probe` on the
first export item. Any failure (symbol missing, layout mismatch, null
pointer) sets a module flag `_xgc_zero_serialization = False`, prints a
warning, and the export continues on the current `writeBinary()` path with
identical behavior. Once an archive is opened in direct-memory mode, any
reader error aborts the export and deletes the partial file (reusing the
existing `save_abc` error path) — no silent mid-export fallback, preserving
the single-writer rule.

No additional checkbox is introduced: the existing "Use C++ acceleration"
checkbox remains the only opt-in, and the probe automatically selects the
direct-memory route when available (confirmed by the developer).

### 4. Dev loop: override + lazy reload + versioned dev DLLs

`_xgc_find_dll()` gains, in order:

1. `XGEN_XGCORE_DLL` environment variable (absolute path);
2. sidecar file `xgcore_dev_path.txt` (searched in the same directories,
   re-read at export time);
3. newest `xgcore_dev_*.dll` in the searched directories;
4. the existing fixed `xgcore.dll` search.

Loading becomes lazy: `_xgc_load(force=False)`; development mode is defined
as "an override or a dev DLL is present". In dev mode `save_abc` calls
`_xgc_load(force=True)` before the export so a rebuilt DLL is picked up;
release mode keeps the current import-time load. The reload must only run
when no `CppArchive` is open (archives are closed at export end; the wrapper
guards and documents this). Old modules are never unloaded — a locked DLL
file simply stays mapped until Maya exits and is harmless.

A new dev build script (`cpp/scripts/build_dev.ps1`) builds into a separate
`cpp/build_dev/` directory and copies the result to
`xgcore_dev_<timestamp>.dll` in the searched staging location, so the linker
never writes to a locked file and each rebuild produces a fresh name.

### 5. Verification harness

Extend the existing read-back dump (printProps-style JSON, already used for
dual-path A/B) with per-frame per-strand root extraction. For animated
exports (spline animation on), the harness reports:

- same-index root displacement (count over epsilon, max, percentiles);
- root-set identity and uniqueness per frame;
- matched-by-root geometry deltas and per-frame CV-count consistency.

The check must interpret animated scenes correctly: roots should be stable,
but strand geometry legitimately changes when the scene's guides are
animated. The scratch prototype (`analyze_roots*.py` in `.data`) becomes the
basis for the formalized script.

## Risks / Trade-offs

- [MSVC/STL ABI mismatch when calling exported members or reading layouts]
  → Build with the same toolset family as each Maya version (VS2017/2019 for
  2019, VS2022 for 2025); prefer exported members; treat `shared_ptr` as
  read-only (never copy the control block); verify `_ITERATOR_DEBUG_LEVEL`
  matches; fall back to `writeBinary()` on any probe failure.
- [Pointer acquisition spike fails] → Documented decision gate: the change
  shrinks to dev loop + verification; the reader entry points remain for a
  later route.
- [Internal layout drift on a future Maya version] → Runtime probe plus a
  documented re-verification checklist (pe_exports.py + offset spot-checks)
  per supported version.
- [Dev reload with open archive handles] → Guard reload to export boundaries;
  document that a mid-export reload is unsupported.
- [Per-frame geometry differences misread as sorting bugs] → The harness
  reports root stability separately from geometry deltas; the A/B parity
  check remains the sorting oracle.

## Migration Plan

No rollout steps: direct-memory mode is opt-in via the existing acceleration
checkbox and defaults to the serialized path on any probe failure. Rollback
is removing the feature flag or the DLL; the serialized path and pure-Python
path remain untouched. Dev-loop behavior only activates when an override or
dev DLL is present, so release users see no change.

## Verified results (Maya 2025, 2026-08-21)

Direct-memory read implemented and verified end to end (scene
`hair_debug.mb`, density 10, 399,474 curves / 1,997,370 CVs):

| Case | Zero-serialization | Serialized accelerated | Pure Python (ref) |
|---|---|---|---|
| Static export | 1.60 s | 5.25 s | 14.7 s |
| Spline animation, 2 frames | 6.30 s | 13.64 s | — |
| DLL data acquisition (read + write) | 0.60 s | writeBinary 3.0 s + parse/write 1.1 s | — |

Output parity: positions / widths / nverts are byte-identical between the
direct-memory and serialized accelerated routes for every sample (verified by
read-back comparison of the exported Alembic files). The root-stability
harness reports identical results on both routes (same-index roots unchanged,
geometry deltas = the scene's animated guide 3 tip, not a sorting issue).

Implementation notes discovered during verification (all confirmed by
disassembly of the 2025 binaries):

- `XgSplineDataItem::positions()` etc. are thin wrappers around `getStream`,
  which walks the item's stream map (key at node +0x20, value at +0x90,
  `_Isnil`-based MSVC tree iteration) and returns the `XgMappableBase*` at
  the node value, or a global "sStreamUndefined" placeholder.
- The mappable's data lives behind the impl's virtual slots: slot 1 returns
  the element count, slot 4 the data pointer. The impl class differs per
  stream: faceUV/faceId/texcoords use `XgVectorMappableImpl<T>` in
  AdskXGen.dll; positions/primitiveInfos/widthPerCV use an xgenToolkit.mll
  impl class with a different internal layout, so raw begin/end reads are not
  portable — the vtable interface is.
- In-memory `PrimitiveInfos` records are 3 uint32s (offset + 64-bit length),
  matching the serialized 12-byte records.
- Bake-UV and ptex-guide-id exports keep the serialized route (they need
  face/root parsing); parity there holds by construction.

## Open Questions

- Runtime confirmation on Maya 2019 requires a 2019 machine; the report's
  layout verification is the evidence until then (the 2019 step is a
  documented follow-up, not a blocker for the 2025 work).
