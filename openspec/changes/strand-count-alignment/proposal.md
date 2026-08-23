## Why

Animated exports currently assume every frame's strand vertex counts match
the first frame: both the Python and C++ paths reuse the first frame's
topology (nverts/orders/knots/widths) and only swap positions. When XGen's
interactive spline produces a different per-strand CV count on a later frame,
the Python path misaligns or overruns its position array, the C++ serialized
and direct-memory paths silently write inconsistent Alembic data, and the
result can fail to import into UE. This change aligns every animated frame's
strand point counts to the first frame (or fails explicitly), so animated
exports are structurally consistent.

## What Changes

- Define a strand resampling strategy that converts each later frame's
  per-strand CV count to the first frame's per-strand CV count (the exact
  interpolation is a design task in this change - the algorithm is not
  finalized here).
- Apply the alignment in both export paths: the Python exporter
  (`XGenProxyEveryFrame`) and the C++ core (serialized
  `xg_abc_write_first`/`xg_abc_write_frame` and direct-memory
  `xg_abc_write_xgen_memory_first`/`xg_abc_write_xgen_memory_frame`).
- When alignment is impossible (for example a strand disappears), fail with a
  clear error instead of writing inconsistent data; the curve-object path
  (`XGenUEGroomExporter`) already rejects count mismatches and stays as is.
- **BREAKING**: none by default; behavior only changes for frames whose
  topology currently differs from the first frame (previously undefined).

## Capabilities

### New Capabilities
- `strand-count-alignment`: per-frame strand point-count alignment to the
  first frame in animated exports.

### Modified Capabilities
- None. The existing `xg-core` and `exporter-export-hygiene` requirements
  remain valid; this change adds a new capability that both export paths
  implement.

## Impact

- `XGenDescriptionUEGroomExporter.py` / `_py2`: strand resampling in
  `XGenProxyEveryFrame.write_frame`.
- `cpp/` (`abc_writer.cpp` and the memory-read path): shared frame writer
  gains topology alignment or explicit mismatch failure.
- `docs/maya-agent-debugging.md` and README: document the alignment behavior
  and remove the "may fail to import because vertex counts differ" known
  issue once verified.
