## 1. xgcore DLL API Extensions

- [ ] 1.1 Add multi-object archive support: `xg_abc_add_curves` returning a
      per-object sub-handle; refactor existing raw-byte first/frame writers to
      sub-handles while keeping the CLI single-object flow working
- [ ] 1.2 Add array-input curve writing: `xg_abc_write_curves_first` /
      `xg_abc_write_curves_frame` accepting positions, nverts, orders, knots,
      widths
- [ ] 1.3 Add generic arbitrary-parameter writes: `xg_abc_write_arb_string` /
      `int32` / `float` / `v2f`
- [ ] 1.4 Extend parse output to expose `FaceId`, `FaceUV`, and the strand
      order (guide-id-from-ptex support)
- [ ] 1.5 Add unit tests for the new APIs, a multi-object CLI demo, rebuild,
      and re-verify `xgcore.dll` has no external dependencies

## 2. Python Wrapper Module

- [ ] 2.1 Implement `xgcore_wrapper.py` (Python 2.7/3.x compatible): DLL
      discovery, ctypes signatures, availability flag
- [ ] 2.2 Implement `CppArchive` mirroring the Python archive/OCurves interface
      used by `save_abc`
- [ ] 2.3 Implement `CppXGenProxy`, `CppCurvesProxy`, `CppGuideProxy` with
      interfaces mirroring the existing proxy classes
- [ ] 2.4 Verify import and a smoke call under Python 3; audit syntax for
      Python 2.7 compatibility

## 3. XGenUEGroomExporter Integration

- [ ] 3.1 Add the "Use C++ acceleration" checkbox, gated on DLL availability
- [ ] 3.2 Branch `save_abc` to use `CppArchive`/Cpp proxies when enabled,
      keeping the original path otherwise
- [ ] 3.3 Ensure every option is honored in accelerated mode (curve/XGen type,
      animation, bake UV, group name/id, guide flag)
- [ ] 3.4 Dual-path A/B: export the same scene with the checkbox off and on and
      compare structure/attributes

## 4. XGenDescriptionUEGroomExporter Integration

- [ ] 4.1 Add the checkbox and branch `save_abc`
- [ ] 4.2 Implement the accelerated description path: ConvertToInteractive +
      `writeBinary()` bytes to the DLL, exposing `hairRootList`, strand order,
      `FaceUV`, `FaceId` for guide ids
- [ ] 4.3 Implement `CppGuideProxy` from `xgmGuideGeom` arrays
- [ ] 4.4 Route `write_guide_id_from_ptex` results (closest guides, weights,
      ids) through the new arb-parameter API
- [ ] 4.5 Verify option parity (spline animation, use guide, card id, group id,
      ptex guide id, bake UV)

## 5. Regression and Handoff

- [ ] 5.1 Run dual-path A/B on a real scene and record results
- [ ] 5.2 Integrate the py2 twins with the same wrapper; test on a real
      Maya 2018/2019 install (developer machine)
- [ ] 5.3 Prepare updated UE import checklist; developer verifies both paths
      import correctly in UE
- [ ] 5.4 Update README and docs: wrapper usage, checkbox, `xgcore.dll`
      distribution

