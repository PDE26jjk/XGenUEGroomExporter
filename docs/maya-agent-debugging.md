# Agent-driven Maya debugging

How an AI agent (or any external Python process) can talk to a running Maya
instance, run code, read results and tracebacks, capture `print()` output, and
drive the XGen export pipeline end to end.

## Communication channel

The bridge uses Maya's built-in TCP **command port**, the same mechanism used
by `MayaLog2Jupyter`:

```text
Agent / external Python                 Maya 2025
  │  socket.connect(127.0.0.1:4434)       │
  │  send  MEL  or  python("...")         │
  │  recv  result / traceback (NUL-terminated)
  │  tail mayalog.txt  <── cmdFileOutput  │  (print output)
```

Maya side (script editor, or `userSetup.py`):

```python
import maya.cmds as cmds
if not cmds.commandPort(":4434", query=True):
    cmds.commandPort(name=":4434")
```

Everything on the Python side lives in
[`maya_bridge.py`](../maya_bridge.py) and uses only the standard library.

## Quick start

```python
from maya_bridge import MayaBridge

bridge = MayaBridge(port=4434, timeout=30)

print(bridge.mel("about -version;"))            # '2025'
print(bridge.python("maya.cmds.ls()"))          # wrapped in python("...")

out = bridge.run_python("import maya.cmds as cmds; cmds.ls()")
# {'ok': True, 'result': ..., 'error': ''}

out = bridge.run_python("1/0")
# {'ok': False, 'error': '执行 MEL 脚本过程中出错\nline 1: division by zero\n# Traceback ...'}

# Capture print() output:
bridge.attach_log(r"R:/path/to/log.txt")
out = bridge.run_python_with_log("print('hello from maya')")
print(out["stdout"])                            # 'hello from maya\n'
bridge.detach_log()
```

Command line:

```bash
python maya_bridge.py --mel "about -version;"
python maya_bridge.py --python "import maya.cmds as cmds; cmds.ls()"
```

## API

| Method | Purpose |
|---|---|
| `mel(code)` | Run raw MEL, return response text |
| `python(code)` | Run Python via the MEL `python("...")` wrapper |
| `run_python(code)` | `{ok, result, error}`; errors carry the full traceback |
| `eval_json(expr)` | Evaluate a one-line expression and parse JSON (see note below) |
| `attach_log(path)` / `detach_log()` | Redirect script-editor output to a file |
| `take_log()` | Return new log bytes since the last read |
| `run_python_with_log(code)` | `{ok, result, error, stdout}` in one call |

## Verified platform facts (Maya 2025, zh_CN on Windows)

- **Single-client port**: the command port accepts one connection at a time.
  A second connection blocks until the first closes. Every call in the bridge
  therefore connects, sends, reads, and closes (serialized by an internal lock).
- **Return values**: `python()` only returns a value for a single *line* of
  code. Multi-line code runs fine but returns an empty response. `eval_json`
  deliberately uses the one-line `import json; json.dumps(...)` form.
- **Errors**: come back over the socket as UTF-8 text with a locale-specific
  MEL prefix (`执行 MEL 脚本过程中出错` for zh_CN, `Error executing MEL script`
  for en_US) followed by the full Python traceback.
- **`print()` output**: does NOT travel over the socket. It goes to Maya's
  script editor; capture it with `cmdFileOutput` (`attach_log`).
- **Large responses**: the protocol streams until the NUL terminator; the
  bridge loops on `recv()` so results are not truncated. (The original
  `MayaLog2Jupyter` kernel reads once with `recv(10240)` and truncates.)
- **Qt bindings**: Maya 2025 uses **PySide6**/`shiboken6`; the exporters fall
  back to PySide2 for older Maya.
- **Import side effect**: importing `XGenDescriptionUEGroomExporter` (and
  `XGenUEGroomExporter`) always creates and shows its Qt window. An agent must
  close these windows after import:

  ```python
  QtWidgets = X.QtWidgets
  app = QtWidgets.QApplication.instance()
  for w in list(app.topLevelWidgets()):
      if w.__class__.__name__ == "SaveXGenDesWindow":
          w.close()
  ```

- **Save dialog**: `cmds.fileDialog2` can be monkey-patched so an agent can
  run `save_abc()` headlessly:

  ```python
  _orig = cmds.fileDialog2
  cmds.fileDialog2 = lambda **kw: ["R:/path/out.abc"]
  try:
      result = win.save_abc()
  finally:
      cmds.fileDialog2 = _orig
  ```

## Driving an XGen export from an agent

Tested against `XGenDescriptionUEGroomExporter` with an interactive
description (`description23`, guides `xgGuide1..3`):

1. Insert the repo into `sys.path` and import the exporter.
2. Close the auto-created window (see above).
3. Select the description: `cmds.select("description23", replace=True)`.
4. Build the export list: `win = X.SaveXGenDesWindow(); win.fillTableWithSelectList()`.
5. Set options per item: `export`, `useGuide`, `bakeUV`, `animation`.
6. Patch `cmds.fileDialog2`, wrap in `cmds.undoInfo(openChunk=True)`, call
   `win.save_abc()`.
7. Read progress and the final `RESULT_PATH` from the captured log.

Recorded run (pure-Python path, static frame):

```text
ConvertToInteractive: 0.72s
writeBinary: 3.05s | flatten: 0.42s | getXgenData: 4.41s
write_first_frame: 19.94s
Data has been saved in .../bridge_agent_test.abc, it took 25.11 seconds.
```

Timing varies with scene state, caches, and machine load: the first
(cold) run measured 25.11 s while warmed runs measure ~14.6 s, matching the
developer's ~14 s measurement.

The same export through the C++ path (`useCpp_cb` checked, `xgcore.dll`
loaded from `MAYA_LOCATION`) took ~9.4 s before optimization and **~4.6 s
after** the unconditional raw-data parses in `write_first_frame` were gated
on `needBakeUV or needGuideIds` (see `XGenDescriptionUEGroomExporter.py`).
Output is identical (48,338,934 bytes vs the pure path's 48,339,148; both
start with the `Ogawa` signature). The common static case is now ~3.2x faster
(14.6 s -> 4.6 s); with bake UV on the parses are restored and the export
takes ~12.6 s.

The export converts the description to interactive XGen and leaves temporary
`z<hash>_description23_splineDescription` nodes under the `_saveXGenDesWindow`
parent. Clean up only what the agent created by diffing `cmds.ls(type='xgmSplineDescription')`
before/after, then deleting the new parents; or use the tool's own
`deleteSaveXGenDesWindowParent()` to clear all temporary data.

### Dual-path A/B read-back comparison

To verify parity between the pure-Python and C++ outputs, dump both Alembic
trees inside Maya (the developer's `printProps` script adapted to JSON: object
hierarchy, schema, curve/vertex counts, widths, arb/user properties, sample
counts) and diff the dumps. See the `Option-parity A/B` section in
`openspec/changes/cpp-export-integration/design.md` for the verified matrix.
This method caught a real parity bug: the C++ `bake_uv` wrote `groom_root_uv`
with the bake-UV checkbox off when ptex guide ids were enabled.

## C++ acceleration and DLL discovery

`XGenUEGroomExporter.py` / `XGenDescriptionUEGroomExporter.py` try to load
`xgcore.dll` at import time via `_xgc_find_dll()`. The candidate directories
are searched in this order:

1. The directory of `__file__` (the script's own directory),
2. Maya's user script directory (`cmds.internalVar(userScriptDir=True)`),
3. `os.getcwd()`,
4. Every entry in `PATH`.

Observed on Maya 2025 (zh_CN): Maya's working directory is
`R:\Program Files\Autodesk\Maya2025` (i.e. `MAYA_LOCATION`), never the script
directory. Worse, when code is run from the Maya script editor / console
instead of a file import, `__file__` is `None` and the first branch is
skipped entirely. In that situation a DLL next to the script is NOT found,
while a DLL in `MAYA_LOCATION` (or in `PATH`) IS found because the cwd branch
matches.

Current verified setup: `xgcore.dll` placed at
`R:\Program Files\Autodesk\Maya2025\xgcore.dll` is discovered because Maya's
cwd is exactly that directory. The agent can verify availability at runtime:

```python
import XGenDescriptionUEGroomExporter as X
print(X._xgc_available, X._xgc_error, X._xgc_find_dll())
```

If `_xgc_available` is `False` after placing the DLL, reload the module
(`importlib.reload(X)`) so the import-time loader re-runs, then close the
window that the reload re-creates.

## Suggested agent debug loop

```text
1. send code (wrapped in try/except inside Maya, or rely on the traceback)
2. read {result, error, stdout}
3. if error: model analyzes traceback, proposes a fix
4. send corrected code, repeat
5. wrap scene-mutating runs in undoInfo chunks; verify state afterwards
```

## Zero-serialization spike (Maya 2025, verified 2026-08-21)

The raw `MPxData*` for `outSplineData` is reachable from Python through the
legacy `maya.OpenMaya` binding, whose SWIG objects expose `.this`:

```python
import maya.OpenMaya as om1
import ctypes

sel = om1.MSelectionList()
sel.add("z..._description23_splineDescriptionShape")  # any xgmSplineDescription
dp = om1.MDagPath()
sel.getDagPath(0, dp)
fn = om1.MFnDependencyNode(dp.node())
plug = fn.findPlug("outSplineData", False)
mobj = plug.asMObject()          # legacy asMObject() returns MObject, no out-arg
mdata = om1.MFnPluginData(mobj)
pdata = mdata.data()
addr = int(pdata.this)           # MPxData*
pdata.thisown                    # False: Maya owns it; Python must not delete it

spline_ptr = ctypes.c_void_p.from_address(addr + 0x10).value  # XgSplineData*
```

Verified on the running scene: `MPxData + 0x10` yields a non-null
`XgSplineData*`, and the vtable address falls inside `xgenToolkit.mll`
(module base `0x7ffab0910000`, vtable `0x7ffab0d29dc0`). `thisown` is
`False`, so the wrapper never frees the Maya-owned object; keep a reference
to `pdata` for the duration of the read. The API 2.0
(`maya.api.OpenMaya`) `MPxData` wrapper does NOT expose `.this` — use the
legacy binding only for address acquisition. `MDataHandle.asAddr()` was
ruled out: its dereference does not point to the `MPxData` object.

**Decision gate (change `xg-zero-serialization-read`, task 1.2): the
zero-serialization route is viable.** The reader lives in `xgcore.dll` and
receives the address as an integer from the snippet above.

## DLL dev loop (verified 2026-08-21, no Maya restart)

The embedded wrapper supports a development iteration loop without
restarting Maya. Discovery order (all four exporter scripts):

1. `XGEN_XGCORE_DLL` environment variable (absolute path);
2. sidecar file `xgcore_dev_path.txt` (first one found in the script dir,
   Maya user script dir, or cwd; re-read at export time);
3. newest `xgcore_dev_*.dll` in the same directories (by mtime);
4. the original fixed `xgcore.dll` search (script dir, userScriptDir, cwd,
   PATH).

In development mode (any of 1-3 present) `save_abc` reloads the DLL before
each export, so a rebuilt DLL is picked up without restarting Maya. Release
behavior is unchanged: one load at script import. Reload is refused while a
`CppArchive` is open.

Build script: `cpp/scripts/build_dev.ps1` builds into `cpp/build_dev/` and
stages `xgcore_dev_<timestamp>.dll` (never touching a locked module).

Verified on the running Maya 2025: after `importlib.reload`, the wrapper
loaded the newest dev DLL; writing the sidecar switched the loaded module to
the pointed-at DLL; deleting the sidecar switched back to the newest dev DLL;
an accelerated export ran end-to-end (4.55 s) through the dev DLL.

## Zero-serialization read (verified 2026-08-21, Maya 2025)

The direct-memory read route is implemented and verified end to end. Layout
facts confirmed by disassembly and live probing:

- `XgSplineDataItem::positions()` / `primitiveInfos()` / `widthPerCV()` are
  thin wrappers over `getStream`, which walks the item's stream map (key at
  node +0x20, value at +0x90) and returns the `XgMappableBase*` or a global
  "sStreamUndefined" placeholder.
- The mappable's data lives behind the impl's virtual slots: slot 1 returns
  the element count, slot 4 the data pointer. The impl class differs per
  stream: faceUV/faceId/texcoords use AdskXGen.dll's `XgVectorMappableImpl<T>`;
  positions/primitiveInfos/widthPerCV use an xgenToolkit.mll impl class.
  Never read begin/end at fixed offsets across stream types — use the vtable
  slots.
- In-memory `PrimitiveInfos` records are 3 uint32s (offset + 64-bit length),
  matching the serialized 12-byte records.
- The slot-1 count can under-report the allocation on some impl classes; the
  primitive records are authoritative, validated with VirtualQuery before
  copying.

Verified parity: static and 2-frame spline-animation exports through the
direct-memory route are byte-identical (positions/widths/nverts, all samples)
to the serialized accelerated route. Timings: static 1.60 s vs 5.25 s;
animation 2 frames 6.30 s vs 13.64 s. Bake-UV and ptex-guide-id exports
intentionally keep the serialized route.

Root-stability regression check (runs inside Maya, uses the alembic bindings):

```python
exec(open(r"R:/.../cpp/scripts/root_stability.py").read())
analyze(r"R:/.../anim.abc", r"R:/.../out.json")
```

Reports same-index root displacement, root-set identity, matched-by-root
geometry deltas, and per-frame CV consistency. Geometry deltas in animated
scenes are expected (e.g. guide 3's tip is keyframed by animCurves), not
sorting failures.

## Refresh DLL, cleanup and undo options (exporter-usability-fixes)

Both exporter windows expose a **Refresh DLL** button that re-runs
`_xgc_load(force=True)` in the current session, updates the "Use C++
acceleration" checkbox, and shows the loaded DLL path or the load error. This
lets a user place or replace `xgcore.dll` after the script was imported
without restarting Maya. On failure the message shows the concrete error and
the full list of searched directories (plus the `XGEN_XGCORE_DLL` escape
hatch), so users know where to put the DLL. The refresh is refused while an
archive is open.

Animated exports now delete each frame's temporary `z<hash>` spline node
after that frame is written and remove all remaining temporary nodes at
export end (both success and failure paths), and run with scene undo disabled
for the duration of the export, restoring the previous undo state afterwards.

The **Keep temporary data** checkbox (default off) preserves the old
behavior for debugging: temporary nodes stay in the scene and undo stays
enabled during export.

Known limitation (Maya 2019 / Python 2.7): re-importing the exporter module
(`reload()` or a fresh `import` after deleting `sys.modules` entries) and
forcing an in-session DLL reload were each followed by crashes
(`xgenToolkit.mll` or `python27.dll` access violations) in agent test
processes. The mechanism is not yet confirmed; test scripts should run in a
freshly started Maya rather than re-importing the module mid-session.
