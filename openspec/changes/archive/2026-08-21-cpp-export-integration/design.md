## Context

See proposal.md - Why. The compiled core (`cpp/`, `xgcore.dll`) is complete and
verified: parser output is byte-identical to the Python reference, Alembic
writing is read-back verified, and a single frame parses+sorts+writes in ~1.2 s
vs ~10 s in Python. The exporters are two UI scripts plus py2 twins, currently
written entirely in Python (alembic binding, imath arrays).

Constraints that shape the design:

- The original pure-Python export path must remain fully functional and is the
  default (user decision: maximum compatibility).
- Users paste the whole exporter script into Maya's script editor and run it,
  so each exporter must remain a single self-contained `.py` file (no separate
  wrapper module to install).
- A single `.abc` file can have exactly one writer for its whole lifetime;
  Python's alembic binding and the DLL cannot both write the same archive.
- One x64 DLL must serve all Maya versions including Python 2.7, so the DLL
  cannot call the Maya API.
- No numpy; no new third-party Python dependencies (ctypes only).

## Goals / Non-Goals

**Goals:**

- Add an opt-in "Use C++ acceleration" checkbox to both exporter windows.
- Route an accelerated export entirely through the DLL with identical options
  and output semantics.
- Keep `save_abc` orchestration shared between both paths via a thin
  abstraction (archive + proxy classes).
- Provide dual-path A/B regression so parity is verifiable.

**Non-Goals:**

- Changing the original Python export behavior or its output.
- Optimizing `writeBinary()` or `ConvertToInteractive` (Maya-side costs).
- Speeding up Python-side reading of hundreds of thousands of converted
  NURBS curves (requires a Maya plugin or tracing back to the source spline;
  noted for a future change).
- pybind11 or numpy.

## Decisions

### 1. Dual-path architecture with a per-export single writer

`save_abc` gains a branch: when the checkbox is on and the DLL loaded, all
proxies are Cpp variants and the archive is a `CppArchive`; otherwise the
original classes run untouched. The two paths never share a file. This is the
only architecture satisfying the single-writer rule while keeping the Python
path intact.

Alternatives considered: making the DLL the only writer (rejected by the user:
keep the original path for maximum compatibility) and mixing writers per object
(impossible - one file, one writer).

### 2. Embedded wrapper per exporter script (single-file delivery)

The wrapper code is embedded in each of the four exporter scripts: DLL
discovery/loading, ctypes signatures, `CppArchive`, and the `CppXGenProxy` /
`CppCurvesProxy` / `CppGuideProxy` classes live inside the script itself and
stay importable under both Python 2.7 and 3.x. This keeps the established
distribution model: users paste the whole `.py` into Maya's script editor and
run, with no additional Python module to install. The cost is deliberate
duplication of the wrapper section across the four scripts. Because a pasted
script has no `__file__`, DLL discovery searches Maya script paths, `PATH`, and
known standard locations instead of the script directory. py2/py3 differences
are confined to small helpers (bytes vs str for ctypes buffers).

### 3. Minimal-diff integration through interface mirrors

The Cpp proxy classes implement the same methods `save_abc` already calls
(`write_group_name`, `write_is_guide`, `write_group_id`, `write_first_frame`,
`write_frame`, `bake_uv`, and the Description-script extras such as
`hairRootList`, `firstSplineIndex2order`, `write_guide_id_from_ptex`). A small
factory picks the class set. In XGenUEGroomExporter the per-row Type option
(xgenType / curveType) selects CppXGenProxy vs CppCurvesProxy exactly as in the
original script; the Description exporter has no such option and keeps its
single proxy flow. `save_abc` keeps its frame loop, group-id pass, bake pass,
and ptex pass; only archive creation and the final close differ (Cpp mode
closes explicitly; Python mode keeps the existing GC semantics).

### 4. xgcore C ABI extensions

- `xg_abc_add_curves(handle, name, is_guide, group_id, &sub)` - one archive,
  multiple top-level curve objects; existing raw-byte first/frame writers move
  to the sub-handle.
- `xg_abc_write_curves_first/sub(frame)` - array input for Maya curves and
  guides: positions, nverts, orders, knots, widths (the DLL does not derive
  knots because Maya curve degree varies).
- `xg_abc_write_arb_string/int32/float/v2f(sub, name, data, n)` - replaces the
  Python `write_param` for `groom_group_cards_id`, `groom_closest_guides`,
  `groom_guide_weights`, `groom_id`, `groom_root_uv`, etc.
- Parse output extension exposing `FaceId`/`FaceUV` and the strand order for
  guide-id-from-ptex (the Description exporter computes guide ids from the
  first spline's UV data).

Existing entry points stay stable; the CLI keeps working with a single implicit
object for backward compatibility.

### 5. UI and failure semantics

Checkbox default off; enabled only when the DLL loads. If a load/parse check
fails at export start, show a warning and run the Python path. If the C++ core
reports an error after the archive is opened, abort with the DLL error message
and delete the partial file - no silent mid-export fallback (a fallback after
creating the archive would violate the single-writer rule).

### 6. Verification: dual-path A/B

The developer exports the same scene with the checkbox off and on, then
compares via the existing xgcore read-back tooling plus Alembic read-back of
the Python-written file: curve counts, per-curve vertex counts, arb
parameters, animation sample count/times. UE import is the final manual check
performed by the developer. Agents SHALL NOT auto-launch Maya or UE; every
step that opens Maya or UE is run manually by a developer.

## Verified Benchmark (2026-08-20)

Scene: `description23` with guides, static frame, bake UV off unless noted.
Driven through the agent bridge (see `docs/maya-agent-debugging.md`); timings
are warmed runs with per-stage timers.

| Path | Total | Breakdown |
|---|---|---|
| Pure Python (warmed) | 14.6 s | ConvertToInteractive 0.64 s, writeBinary 2.67 s, Python pack + Alembic write ~11.3 s |
| C++ before optimization | 9.4 s | convert 0.78 s, writeBinary 3.33 s, dll write_first 1.06 s, dll parse_frame 2.17 s, dll parse_face 2.04 s |
| C++ after optimization | 4.6 s | convert 0.63-0.68 s, writeBinary 2.8-2.9 s, dll write_first 1.07-1.15 s (parses skipped) |
| C++ with bake UV on | 12.6 s | parses restored (parse_frame 1.85 s, parse_face 1.75 s) plus root-list/bake work |

Findings:

- The earlier 25.11 s pure-Python figure was a cold first run; warmed runs
  measure ~14.6 s, matching the developer's ~14 s measurement.
- Before optimization the accelerated path spent ~4.1 s on two unconditional
  DLL parses of the raw spline data even when bake UV and guide-id-from-ptex
  were off. The parses are now gated on `needBakeUV or needGuideIds`, cutting
  the accelerated export from 9.4 s to ~4.6 s (~2x) with identical output
  (48,338,934 B vs the pure path's 48,339,148 B; both Ogawa-valid).
- Remaining accelerated time is dominated by Maya-side costs: writeBinary
  (~2.9 s) and ConvertToInteractive (~0.7 s), both outside this change's scope
  (design non-goals); the DLL core write itself is ~1.1 s as designed.
- The common static case is now ~3.2x faster (14.6 s -> 4.6 s).

## Option-parity A/B (2026-08-20)

Both paths were exported on the same scene (`description23` + guides) and the
resulting Alembic trees compared with a printProps-style dump (adapted from
the developer's read-back script): object hierarchy, schema, curve counts,
per-curve vertex counts, widths, arb/user properties, and sample counts.

| Option combination | Pure Python | C++ | Structural diff |
|---|---|---|---|
| static + card id + custom group | 15.2 s | 4.8 s | 0 differences |
| animation, 2 frames | 18.6 s | 7.6 s | 0 differences |
| bake UV on | 19.7 s | 12.7 s | 0 differences |
| ptex guide id (before fix) | 20.8 s | 20.3 s | 1 difference |
| ptex guide id (after fix) | 20.8 s | 16.2 s | 0 differences |
| spline animation, 2 frames | 34.3 s | 15.0 s | 0 differences |

The A/B run exposed one parity bug: `CppCurvesProxy.bake_uv` wrote
`groom_root_uv` whenever `hairRootList` and a bake mesh existed, even with the
bake-UV checkbox off (triggered when ptex guide ids were enabled). The pure
Python path checks `needBakeUV`; the C++ path did not. Fixed by adding the
same guard to `CppCurvesProxy.bake_uv` in all four exporter scripts, and
re-verified with 0 differences (16.2 s vs 20.3 s - the fix also removed a
needless ~399k-point UV bake loop).

Per-object parity numbers (hair): 399,474 curves / 1,997,370 vertices, widths
present; guide: 3 curves / 12 vertices, no widths - identical in both paths.

## Risks / Trade-offs

- [A Cpp proxy misses an option the Python path honors] -> Feature-parity
  checklist per script plus dual-path A/B; each option is exercised in both
  modes.
- [The two paths produce byte-different files (Alembic version differences)] ->
  Parity is judged by structure, attributes, and UE import, not byte equality.
- [Mid-export C++ failure leaves a partial file] -> Delete the partial file and
  surface the DLL error message.
- [Python reading of large converted NURBS curve sets stays slow] -> Accepted
  for this change; reading stays in Python (Maya API), writing goes through the
  DLL. A Maya-plugin reader is a possible future change.
- [py2 ctypes differences] -> Wrapper confined helpers; test on a real
  Maya 2018/2019 install (developer machine).

## Migration Plan

No rollout steps: the checkbox defaults off, so existing users see no change.
Users adopt acceleration per export; rollback is unchecking the box or removing
the DLL. `xgcore.dll` is distributed next to the scripts as an optional
artifact.

## Open Questions

- Whether to persist the acceleration preference across sessions: deferred,
  purely a UI convenience; the checkbox stays per-export for now.
