## 1. Algorithm Design

- [ ] 1.1 Select and document the strand resampling algorithm: per-strand
      interpolation (e.g. linear / Catmull-Rom on chord length) to the first
      frame's CV count, endpoint preservation, even CV distribution, and
      fallback for low-CV strands; record the decision in design.md
- [ ] 1.2 Define degenerate-case handling: missing strands, invalid lengths,
      and the exact error message for the explicit-failure requirement

## 2. Python Implementation

- [ ] 2.1 Implement per-strand resampling in
      `XGenProxyEveryFrame.write_frame` (both `XGenDescriptionUEGroomExporter.py`
      and `_py2`), aligned to the first frame's per-strand nverts

## 3. C++ Implementation

- [ ] 3.1 Implement per-strand resampling in the shared frame writer
      (`write_frame_sample` / `build_output_arrays`) for both the serialized
      and direct-memory routes, using the first sample's nverts
- [ ] 3.2 Rebuild `xgcore.dll` and verify exports resolve the new symbols

## 4. Consistency and Failure Paths

- [ ] 4.1 Ensure the Python and C++ paths produce identical resampled output
      for the same input (dual-path A/B on an animated scene)
- [ ] 4.2 Implement explicit failure (clear error + partial archive removal)
      when a strand cannot be aligned, in both paths

## 5. Verification

- [ ] 5.1 Verify byte-identical output on scenes whose frames already match
      topology (Maya 2019 and 2025)
- [ ] 5.2 Construct or find an animated scene with changing strand CV counts
      and verify read-back structural consistency and correct resampling
- [ ] 5.3 Update `docs/maya-agent-debugging.md` and README: alignment
      behavior, algorithm reference, and removal of the vertex-count known
      issue once verified
