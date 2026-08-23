## Context

See proposal.md - Why. Current behavior in both export paths:

- `XGenProxyEveryFrame.write_frame` (Python) reuses the first frame's
  `nverts/knots/orders/widths` (`firstSamp`) and writes the current frame's
  positions into an array sized and offset by the first frame's
  `sorted_offset_map`; a differing strand length misaligns data and a larger
  total CV count overruns the array.
- `write_frame_sample` (C++, shared by the serialized and direct-memory
  routes) reuses `w->nverts/orders/knots/widths` from the first sample and
  builds positions from the current frame with no topology check.
- `abc_write_curves_frame` (curve-object path, `XGenUEGroomExporter`)
  already rejects a total CV-count mismatch with a clear error.

## Goals / Non-Goals

**Goals:**

- Make animated samples structurally consistent with the first frame in both
  export paths, whether by resampling to the first frame's strand counts or
  by explicit failure when that is impossible.
- Keep byte-identical output for frames whose topology already matches.

**Non-Goals:**

- Finalizing the resampling algorithm now. The exact interpolation (linear,
  Catmull-Rom, chord-length parameterization, etc.) and its edge cases are a
  design task in this change and must be decided during implementation with
  verification on real animated scenes.
- Changing the curve-object path (`XGenUEGroomExporter`), whose mismatch
  check already satisfies the failure requirement.

## Decisions

### 1. Alignment lives in the shared frame writer, per strand

Both routes should resample per strand: for each strand in a later frame,
produce exactly `first_nverts[strand_index]` CVs from the frame's strand
geometry. In Python this replaces the current offset-map fill; in C++ it is a
step in `build_output_arrays` / `write_frame_sample` that takes the first
frame's nverts as a parameter. The curve-object path keeps its existing
strict count check.

### 2. Candidate algorithms (to be finalized in implementation)

- Resample each strand independently to the target CV count using an
  interpolation over the strand's parameterization (for example linear or
  Catmull-Rom on chord length), preserving the first and last CVs (hair root
  and tip) and distributing the remaining CVs evenly.
- If a strand's source CV count is below the minimum for the chosen
  interpolation, fall back to linear subdivision; if a strand is missing
  entirely, fail per the spec's explicit-failure requirement.

### 3. Determinism and parity

The resampling must be deterministic and identical across the Python and C++
paths for the same input so dual-path A/B comparison remains valid; the
algorithm choice must be recorded in the code and docs once selected.

## Risks / Trade-offs

- [Resampling changes strand shape] → interpolation preserves endpoints and
  is verified by read-back; A/B parity is only guaranteed for frames that
  already match topology.
- [Per-frame cost increase] → resampling is O(total CVs) per frame; measured
  and recorded during verification.
- [Endless edge cases (degenerate strands, disappearing strands)] → handled
  by the explicit-failure requirement and verified on animated test scenes.

## Migration Plan

No rollout steps: exports whose frames already match topology are
byte-identical; exports with mismatched topology change from undefined
(misaligned/silently inconsistent) to aligned or explicitly failed.

## Open Questions

- Exact interpolation algorithm and parameterization (decided during apply,
  with animated-scene verification).
- Whether resampling should be optional (a checkbox) or always on; the spec
  currently requires alignment, so this would be a spec change if optional.
