# xg-core Specification

## Purpose

Lets the XGen groom exporter offload parsing of XGen spline data and writing
of Alembic groom files to a compiled core, so large hair exports complete in
a fraction of the current time.

## Requirements

### Requirement: Parse XGen spline data binary

The core SHALL parse a raw `outSplineData` byte stream into typed item
payloads. The stream SHALL be decoded as a sequence of blocks with a JSON
header followed by per-group payloads that may be zlib-deflated; each group
SHALL contain inner blocks whose payloads are addressed by (group, index)
references from the header JSON. The core SHALL support both the deflated and
the non-deflated group formats. When the header requests an unsupported
format, the core SHALL return a clear error.

#### Scenario: Parse the reference sample
- **WHEN** the core parses the provided reference `raw_data.bin` sample (14
  groups, GroupDeflate=true)
- **THEN** it reports 399,874 curves and 1,999,370 CVs, and payload counts
  for `PrimitiveInfos`, `Positions`, `WIDTH_CV`, `FaceId`, and `FaceUV`
  matching the reference

#### Scenario: Non-deflated groups
- **WHEN** the header sets GroupDeflate=false
- **THEN** the core reads group payloads directly from their block ranges
  without inflating

#### Scenario: Unsupported base64 groups
- **WHEN** the header sets GroupBase64=true
- **THEN** the core returns an error code and a message identifying the
  unsupported format

### Requirement: Reorder strands by UV

For exports that require a stable strand order, the core SHALL compute a
permutation that orders strands lexicographically by (faceId, u, v), breaking
ties by original index, with identical semantics to the Python exporter's
strand ordering.

#### Scenario: Permutation matches the Python reference
- **WHEN** the core computes the strand permutation for the reference sample
- **THEN** the permutation is identical to the reference output produced by
  the Python exporter logic

### Requirement: Write Alembic groom archives

The core SHALL write Alembic `.abc` files containing curves with cubic
B-spline basis, non-periodic wrap, per-curve orders/knots/vertex counts,
vertex-scope width parameters, and the arbitrary geometry parameters
`groom_group_name`, `groom_guide`, `groom_group_id`, and `groom_root_uv`.
When animation is enabled, the archive SHALL contain one sample per exported
frame at the configured time sampling.

#### Scenario: Static groom archive
- **WHEN** the core writes a non-animated archive from a parsed frame
- **THEN** an Alembic reader reports the same curve count, per-curve vertex
  counts, widths, and arbitrary parameters as the Python exporter

#### Scenario: Animated groom archive
- **WHEN** the core writes an animated archive with a time sampling and
  per-frame position samples
- **THEN** the archive exposes one curves sample per frame at the requested
  times

### Requirement: Expose a pure C ABI

The compiled core SHALL expose a C ABI (unmangled exports, no Python runtime
dependency) callable from Python 2.7 and Python 3.x through ctypes. All
functions SHALL return an integer status, and failures SHALL be retrievable
as a message string.

#### Scenario: Loadable from both Python generations
- **WHEN** Python 2.7 and Python 3.x load the DLL through ctypes and call
  the parse entry point
- **THEN** both succeed and produce identical results

#### Scenario: Error reporting
- **WHEN** a call fails, for example on malformed input
- **THEN** the function returns a non-zero status and the error message
  function returns a non-empty description
