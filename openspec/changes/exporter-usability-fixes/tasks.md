## 1. Refresh DLL Button

- [ ] 1.1 Add a "Refresh DLL" button to `XGenDescriptionUEGroomExporter.py`
      and `XGenUEGroomExporter.py`: on click call `_xgc_load(force=True)`,
      update `useCpp_cb.setEnabled(_xgc_available)`, and display
      `_xgc_loaded_path` or `_xgc_error`
- [ ] 1.2 Mirror the button in both `_py2` twins and statically audit the
      Python 2.7 syntax
- [ ] 1.3 Runtime-verify the refresh on one Maya instance: import with no
      DLL available, place a DLL, click refresh, and confirm the checkbox
      enables without a restart; confirm a failed refresh keeps the
      pure-Python default and shows the error

## 2. Temporary Spline Cleanup

- [ ] 2.1 Make the proxies track the current temporary spline parent created
      by `ConvertToInteractive`; delete the previous frame's node after each
      `write_first_frame` / `write_frame` completes
- [ ] 2.2 Add export-end cleanup (finally block) that removes the remaining
      temporary nodes and the exporter's temporary parent, scoped to the
      exporter's own `z<hash>` nodes; verify `bake_uv` / ptex guide-id
      stages do not reference a deleted node
- [ ] 2.3 Mirror cleanup in all four exporter scripts
- [ ] 2.4 Runtime-verify: static and animated exports leave zero temporary
      nodes behind (success and failure paths), outputs are unchanged, and a
      long animated export keeps the temporary-node count bounded

## 3. Disabled-Undo Scope

- [ ] 3.1 Wrap `save_abc` in a disabled-undo scope: read the current undo
      state, disable it for the export, restore it in a finally block
- [ ] 3.2 Runtime-verify on Maya 2019 and 2025 that animated exports run
      correctly with undo disabled and produce identical output; if
      `xgmGroomConvert` fails with undo disabled, fall back to a per-frame
      scope and record the finding
- [ ] 3.3 Mirror the scope in all four exporter scripts

## 4. Keep Temporary Data Option

- [ ] 4.1 Add a window-level "Keep temporary data" checkbox (default off) to
      both exporter windows; when checked, skip cleanup and the disabled-undo
      scope
- [ ] 4.2 Mirror the option in the `_py2` twins
- [ ] 4.3 Runtime-verify the option toggles behavior: on = old behavior
      (nodes and undo records remain), off = cleanup and undo scope apply

## 5. Documentation

- [ ] 5.1 Update `docs/maya-agent-debugging.md`: refresh-button usage, the
      cleanup / disabled-undo behavior, and the keep-temporary-data option
