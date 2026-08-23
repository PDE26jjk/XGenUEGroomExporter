## Why

The exporter UI leaves users stuck with two runtime problems: the "Use C++
acceleration" checkbox is decided once at script import and cannot be
re-enabled after a DLL is placed or replaced, and long animated exports
accumulate a temporary spline node per frame plus unbounded undo records,
which grows Maya's memory (and, in observed cases, fills the system drive)
for the rest of the session.

## What Changes

- Add a "Refresh DLL" button to both exporter windows (all four scripts:
  `XGenDescriptionUEGroomExporter.py` / `_py2` and
  `XGenUEGroomExporter.py` / `_py2`). Clicking it re-runs
  `_xgc_load(force=True)` in both release and development modes, updates the
  "Use C++ acceleration" checkbox state, and shows the loaded DLL path or the
  load error.
- Delete the temporary `ConvertToInteractive` spline nodes per frame during
  animated exports: each frame's spline description is read and written, then
  removed before the next frame is converted, and all remaining temporary
  nodes are removed at export end.
- Wrap each export in a disabled-undo scope (`undoInfo(state=False)` with a
  guaranteed restore) so `xgmGroomConvert` calls stop feeding the undo queue.
- Add a "Keep temporary data" option (default off) that disables the cleanup
  and undo changes so the intermediate scene state can be inspected for
  debugging.
- **BREAKING**: none. Default behavior only becomes cleaner; the pure-Python
  path, C++ path, output parity, and all existing options are unchanged.

## Capabilities

### New Capabilities
- `exporter-export-hygiene`: lifecycle of temporary scene data created during
  export (per-frame conversion nodes), undo handling, and the
  keep-temporary-data debug option.

### Modified Capabilities
- `xg-dll-discovery`: adds a runtime DLL refresh action so the acceleration
  checkbox can be re-evaluated after the script is imported.

## Impact

- The four exporter scripts: new button and checkbox widgets, `save_abc`
  cleanup and undo-scope logic, and the shared `_xgc_load(force=True)`
  refresh path.
- No third-party Python dependencies and no `xgcore.dll` changes; the DLL
  search/override semantics are reused unchanged.
- `docs/maya-agent-debugging.md` gains the refresh-button and
  cleanup/undo-option notes.
