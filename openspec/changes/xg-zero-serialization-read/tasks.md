## 1. Spike and Decision Gate

- [x] 1.1 Spike on the running Maya 2025 instance: obtain the raw `MPxData*`
      address from `MFnPluginData.data()` in Python (probe `.this` and other
      SWIG pointer forms); record the result in
      `docs/maya-agent-debugging.md`
- [x] 1.2 Decision gate: record whether the zero-serialization route is
      viable; if the pointer cannot be obtained, the change scope reduces to
      the dev loop + verification harness (the specs' fallback behavior
      already covers the reduced scope)

## 2. DLL Development Loop

- [x] 2.1 Extend `_xgc_find_dll()` in all four exporter scripts:
      `XGEN_XGCORE_DLL` env var -> sidecar `xgcore_dev_path.txt` (re-read at
      export time) -> newest `xgcore_dev_*.dll` -> existing fixed
      `xgcore.dll` search
- [x] 2.2 Implement lazy `_xgc_load(force=False)`; in development mode reload
      before each export so a rebuilt DLL is picked up without restarting
      Maya; guard against reload while a `CppArchive` is open; keep the
      import-time single load in release mode
- [x] 2.3 Add `cpp/scripts/build_dev.ps1` that builds into a separate
      `cpp/build_dev/` directory and copies the result to
      `xgcore_dev_<timestamp>.dll` in the searched staging location, never
      overwriting a locked module
- [x] 2.4 Verify the agent-driven iteration loop on the running Maya 2025
      without a restart: rebuild -> new dev DLL -> next export uses it
      (record the recipe in `docs/maya-agent-debugging.md`)

## 3. xgcore Reader (zero-serialization)

- [x] 3.1 Add C ABI declarations in `cpp/include/xgcore/xgcore.h`:
      `xg_read_xgen_probe`, `xg_abc_write_xgen_memory_first`,
      `xg_abc_write_xgen_memory_frame`, with stable status codes and
      thread-local error messages
- [x] 3.2 Implement XGen symbol resolution via `GetProcAddress` (version-stable
      mangled names) and the documented layout reads: `MPxData + 0x10` ->
      `XgSplineData*`, item enumeration (prefer exported `itemCount` +
      `getItemByGenIndex`), stream reads (prefer exported
      `positions()`/`widthPerCV()`/`faceUV()`/`faceId()`/`primitiveInfos()`),
      and impl data access via virtual slots 1/4 (works for both the
      AdskXGen.dll and xgenToolkit.mll impl classes)
- [x] 3.3 Fill the same flat arrays the current parser produces (positions,
      widths, face ids/uvs, primitive records, strand order) and reuse the
      existing UV sort + Alembic writer unchanged
- [x] 3.4 Add unit tests with a captured/synthetic in-memory `XgSplineData`
      layout, rebuild, and re-verify `xgcore.dll` has no external
      dependencies beyond the OS

## 4. Python Integration

- [x] 4.1 Wrapper: acquire the `MPxData*` address, call `xg_read_xgen_probe`
      before opening an archive, expose the zero-serialization availability
      flag, and print a fallback warning when the probe fails
- [x] 4.2 Switch `CppXGenProxyEveryFrame` / `CppCurvesProxy` data source to
      the memory entry points when the probe succeeds; keep the
      `writeBinary()` path otherwise; preserve the abort-with-delete error
      handling for mid-export read failures
- [x] 4.3 Mirror the wrapper changes in all four exporter scripts
      (`XGenDescriptionUEGroomExporter.py`, `XGenUEGroomExporter.py` and
      `_py2` twins) and statically audit Python 2.7 syntax
- [x] 4.4 Dual-path A/B on Maya 2025: static and spline-animation exports
      through both routes are byte-identical (positions/widths/nverts, all
      samples). Bake UV and ptex guide ids intentionally use the serialized
      route (the memory path requires face/root parsing, so parity holds by
      construction); structural read-back dumps show zero differences

## 5. Verification Harness

- [x] 5.1 Formalize the root-stability analyzer (per-frame per-strand roots,
      same-index root displacement counts/percentiles, root-set identity,
      matched-by-root geometry deltas, per-frame CV-count consistency) into
      the verification tooling
- [x] 5.2 Run the harness on spline-animation exports from both accelerated
      routes and record results; interpret geometry deltas as scene animation
      (guide 3 tip is animated by animCurves) rather than sorting failures
- [x] 5.3 Wire the harness into the read-back comparison workflow documented
      in `docs/maya-agent-debugging.md` and the A/B recipe

## 6. Profiling and Documentation

- [x] 6.1 Profile per-frame stage timings before/after the change
      (ConvertToInteractive, direct read, sort, Alembic write) at density 10
      and record the breakdown in design.md
- [ ] 6.2 Update `cpp/README.md` and `docs/maya-agent-debugging.md`: dev-loop
      recipe, zero-serialization usage, fallback semantics, and the layout /
      symbol re-verification checklist (`pe_exports.py` per supported Maya
      version)
- [ ] 6.3 Maya 2019 runtime verification: requires a 2019 machine; the IDA
      report's layout evidence covers it until then (documented follow-up,
      not a blocker for the Maya 2025 work)
