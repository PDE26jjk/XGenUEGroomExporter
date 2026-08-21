## 1. xgcore DLL API Extensions

- [x] 1.1 Add multi-object archive support: `xg_abc_add_curves` returning a
      per-object sub-handle; refactor existing raw-byte first/frame writers to
      sub-handles while keeping the CLI single-object flow working
- [x] 1.2 Add array-input curve writing: `xg_abc_write_curves_first` /
      `xg_abc_write_curves_frame` accepting positions, nverts, orders, knots,
      widths
- [x] 1.3 Add generic arbitrary-parameter writes: `xg_abc_write_arb_string` /
      `int32` / `float` / `v2f`
- [x] 1.4 Extend parse output to expose `FaceId`, `FaceUV`, and the strand
      order (guide-id-from-ptex support)
- [x] 1.5 Add unit tests for the new APIs, a multi-object CLI demo, rebuild,
      and re-verify `xgcore.dll` has no external dependencies

## 2. Embedded Acceleration Wrapper

- [x] 2.1 Implement the wrapper section inside each exporter script
      (Python 2.7/3.x compatible): DLL discovery, ctypes signatures,
      availability flag
- [x] 2.2 Implement `CppArchive` mirroring the Python archive/OCurves interface
      used by `save_abc`
- [x] 2.3 Implement `CppXGenProxy`, `CppCurvesProxy`, `CppGuideProxy` with
      interfaces mirroring the existing proxy classes
- [ ] 2.4 Verify the embedded wrapper runs from Maya's script editor (no
      `__file__`) and finds the DLL via standard search paths; audit syntax for
      Python 2.7 compatibility. The agent runs the no-`__file__` check on the
      developer-provided Maya instance through the bridge and statically audits
      py2 syntax; runtime py2 verification waits for the low-version port. See
      `docs/maya-agent-debugging.md` for the verified `__file__`/cwd behavior

## 3. XGenUEGroomExporter Integration

- [x] 3.1 Add the "Use C++ acceleration" checkbox, gated on DLL availability
- [x] 3.2 Branch `save_abc` to use `CppArchive`/Cpp proxies when enabled,
      keeping the original path otherwise
- [x] 3.3 Ensure every option is honored in accelerated mode (curve/XGen type,
      animation, bake UV, group name/id, guide flag)
- [x] 3.4 Dual-path A/B: export the same scene with the checkbox off and on and
      compare structure/attributes. The agent exports both paths on the
      running Maya instance and compares the Alembic trees with the
      developer's read-back script (printProps-style dump + diff). See
      `docs/maya-agent-debugging.md` for the driving recipe

## 4. XGenDescriptionUEGroomExporter Integration

- [x] 4.1 Add the checkbox and branch `save_abc`
- [x] 4.2 Implement the accelerated description path: ConvertToInteractive +
      `writeBinary()` bytes to the DLL, exposing `hairRootList`, strand order,
      `FaceUV`, `FaceId` for guide ids
- [x] 4.3 Implement `CppGuideProxy` from `xgmGuideGeom` arrays
- [x] 4.4 Route `write_guide_id_from_ptex` results (closest guides, weights,
      ids) through the new arb-parameter API
- [x] 4.5 Verify option parity (spline animation, use guide, group name, card
      id, group id, ptex guide id, guide weights, bake UV). The agent exports
      each option combination in both paths on the running instance and
      compares attribute trees; options that cannot be exercised on the current
      scene (e.g. ptex without a ptex file) are recorded as unverified

## 5. Regression and Handoff

All Maya testing runs on Maya instances already started by the developer and
is driven through the agent bridge (`maya_bridge.py`); the developer provides
the command-port number for each instance (Maya 2025 now, low-version later).
UE steps are performed manually by the developer. An agent SHALL NOT
auto-launch Maya or UE.

- [x] 5.1 Agent runs dual-path A/B on the developer-provided Maya instance(s)
      and records results (recipe and read-back comparison:
      `docs/maya-agent-debugging.md`, benchmark section of design.md)
- [ ] 5.2 Integrate the py2 twins with the same embedded wrapper; the agent
      tests them on the developer-provided low-version Maya (2018/2019)
      command port
- [ ] 5.3 Prepare updated UE import checklist; developer verifies both paths
      import correctly in UE
- [x] 5.4 Update README and docs: wrapper usage, checkbox, `xgcore.dll`
      distribution

## 6. Accelerated Export Performance Profiling

- [x] 6.1 Profile the accelerated Description export end-to-end with
      per-stage timings (ConvertToInteractive, writeBinary, parse/sort, Cpp
      archive writes, guide/arb writes) and produce a bottleneck breakdown for
      the observed ~8 s end-to-end run (vs ~1.2 s DLL core time)
- [x] 6.2 Decide whether the remaining cost is Maya-side (ConvertToInteractive
      / writeBinary / Python-side NURBS reading) or DLL-side; record the
      evidence in the change and keep optimizing Maya-side costs out of scope
      per design.md non-goals (future change)
