## Context

See proposal.md - Why. The compiled core (`cpp/`, `xgcore.dll`) is complete and
verified: parser output is byte-identical to the Python reference, Alembic
writing is read-back verified, and a single frame parses+sorts+writes in ~1.2 s
vs ~10 s in Python. The exporters are two UI scripts plus py2 twins, currently
written entirely in Python (alembic binding, imath arrays).

Constraints that shape the design:

- The original pure-Python export path must remain fully functional and is the
  default (user decision: maximum compatibility).
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

### 2. Shared wrapper module `xgcore_wrapper.py`

One module next to the scripts, written to be importable by both Python 2.7
and 3.x, owns: DLL discovery/loading, ctypes signatures, `CppArchive`, and the
`CppXGenProxy` / `CppCurvesProxy` / `CppGuideProxy` classes. The four scripts
import it under `try/except ImportError`. py2/py3 differences are confined to
small helpers (bytes vs str for ctypes buffers).

### 3. Minimal-diff integration through interface mirrors

The Cpp proxy classes implement the same methods `save_abc` already calls
(`write_group_name`, `write_is_guide`, `write_group_id`, `write_first_frame`,
`write_frame`, `bake_uv`, and the Description-script extras such as
`hairRootList`, `firstSplineIndex2order`, `write_guide_id_from_ptex`). A small
factory picks the class set. `save_abc` keeps its frame loop, group-id pass,
bake pass, and ptex pass; only archive creation and the final close differ
(Cpp mode closes explicitly; Python mode keeps the existing GC semantics).

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

Export the same scene with the checkbox off and on, then compare via the
existing xgcore read-back tooling plus Alembic read-back of the Python-written
file: curve counts, per-curve vertex counts, arb parameters, animation sample
count/times. UE import is the final manual check.

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

