## Context

See proposal.md - Why. Relevant current state:

- The "Use C++ acceleration" checkbox is initialized from `_xgc_available`
  once at script import (`useCpp_cb.setEnabled(_xgc_available)`); only dev
  mode re-runs `_xgc_load(True)` before each export, so a release user who
  adds or replaces a fixed-name `xgcore.dll` after import cannot enable the
  checkbox without restarting Maya.
- `ConvertToInteractive` (in both exporter scripts) creates a
  `z<hash>_..._splineDescription` node under a shared temporary parent
  (`getSaveXGenDesWindowParent()`). Animated exports call it once per frame;
  the nodes are only removed by the manual clear-temp action, so long exports
  accumulate nodes and undo records.
- No undo scoping exists in `save_abc`; every `xgmGroomConvert` call writes
  an undo record.
- The C++ memory-read path finishes reading a frame's data inside
  `write_first_frame` / `write_frame`, so a frame's spline node is not needed
  after those calls complete (exception: bake-UV / ptex guide ids parse raw
  data in the same call).

## Goals / Non-Goals

**Goals:**

- Let a user re-resolve the DLL and re-enable the acceleration checkbox
  without restarting Maya, in both release and dev modes.
- Bound the scene/undo growth of animated exports by cleaning per-frame
  temporary nodes and disabling undo during export, with a debug escape
  hatch that preserves today's behavior.

**Non-Goals:**

- Changing DLL discovery order, output parity, or the pure-Python / C++
  export paths.
- Investigating the exact mechanism of the reported C-drive fill beyond what
  this cleanup makes controllable (the keep-temp option lets users reproduce
  and the undo/cleanup changes remove the two prime suspects).

## Decisions

### 1. Refresh button reuses `_xgc_load(force=True)`

One "Refresh DLL" button per exporter window calls `_xgc_load(True)` (which
already guards against reload while an archive is open), then updates
`useCpp_cb.setEnabled(_xgc_available)` and shows `_xgc_loaded_path` or
`_xgc_error` in a status label / message box. This reuses the existing
discovery and error plumbing and works identically in release and dev modes.
Alternatives: auto-rescan on window focus (surprising, runs LoadLibrary
without user intent) and requiring a restart (rejected - it is the pain
point).

### 2. Per-frame cleanup, not export-end-only cleanup

The proxy keeps a reference to its current temporary spline parent. After a
frame is written (`write_first_frame` / `write_frame` complete), the previous
frame's node is deleted via `cmds.delete`; the last frame's node is deleted
when the export call unwinds (finally block), together with the temporary
parent if empty. Deletion happens strictly after the C++ read/write call so
no dangling memory access is possible. Alternatives: cleanup only at export
end (still leaves N nodes resident during a long export - misses the memory
goal) and reusing a single converted node across frames (unverified XGen
behavior, riskier).

### 3. Disabled-undo scope around the whole export

`save_abc` reads `cmds.undoInfo(query=True, state=True)`, sets state to
False for the duration of `_save_abc_impl`, and restores it in a finally
block so success and failure both restore undo. The keep-temp option skips
this scope.

### 4. Keep-temporary-data option gates both behaviors

A window-level checkbox (default off) is consulted by `save_abc`: when
checked, neither cleanup nor undo disabling runs, preserving the exact
current behavior for debugging.

## Risks / Trade-offs

- [Deleting a frame's spline node while a later stage still needs it] →
  deletion is placed after the read/write call, and bake-UV / ptex parsing
  completes inside the same call; a task verifies `bake_uv` does not touch
  the deleted node.
- [`xgmGroomConvert` misbehaves with undo disabled] → a task runs an animated
  export with the disabled-undo scope on both Maya versions; if it fails, the
  scope is applied only around the conversion call with undo re-enabled
  between frames.
- [Refresh loads a DLL whose file is locked by an earlier LoadLibrary] →
  LoadLibrary of the same path returns the existing module; replacing a
  locked DLL on disk remains unsupported (unchanged behavior, already
  documented).
- [Cleanup deletes user scene data by mistake] → deletion is scoped to the
  `z<hash>` prefix and the exporter's own temporary parent only.

## Migration Plan

No rollout steps: default behavior becomes cleaner (cleanup + undo scope),
and the keep-temp option restores old behavior on demand. No DLL or output
format changes; rollback is removing the option and button code, leaving the
export paths untouched.
