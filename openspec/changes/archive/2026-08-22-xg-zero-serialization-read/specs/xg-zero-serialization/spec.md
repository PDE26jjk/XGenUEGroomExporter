## Purpose

Lets the C++-accelerated export read live XGen spline data directly from
memory, skipping serialization, while preserving output parity and falling
back to the serialized path whenever the direct read is unavailable.

## ADDED Requirements

### Requirement: Direct-memory read availability probe

The accelerated export SHALL probe the direct-memory read route before
opening an Alembic archive. The probe SHALL succeed only when the live
`MPxData` pointer can be obtained from Python and the XGen in-memory layout
matches the expected layout on the current Maya version. When the probe
fails, the export SHALL run the existing serialized accelerated path
(`writeBinary` + parse) with the same options and output.

#### Scenario: Probe succeeds
- **WHEN** an accelerated export starts on a Maya version whose XGen in-memory
  layout matches the reader's expectations and the `MPxData` address is
  available
- **THEN** the export reads spline streams directly from memory and never
  calls `writeBinary()`

#### Scenario: Probe fails
- **WHEN** the pointer is unavailable or the layout probe reports a mismatch
  on the current Maya version
- **THEN** the export runs the serialized accelerated path, prints a warning,
  and produces the same output as before

### Requirement: Output parity in direct-memory mode

The direct-memory mode SHALL produce the same Alembic structure and
attributes as the serialized path for the same scene and options: the same
top-level objects, curve counts, per-curve vertex counts, knots, orders,
widths, arbitrary parameters (group name, group id, card id, guide flag,
root UV, guide ids, guide weights), and the same per-frame sample structure.
Arbitrary-property ordering MAY differ between the two paths.

#### Scenario: Same scene, both paths
- **WHEN** the same scene is exported once through the serialized path and
  once through the direct-memory path
- **THEN** the read-back comparison reports zero structural differences

#### Scenario: Every option honored
- **WHEN** a user exports with guides, group name/id, card id, bake UV, ptex
  guide ids, animation, and spline animation enabled in direct-memory mode
- **THEN** all corresponding attributes are present and match the serialized
  path

### Requirement: Single writer preserved

In direct-memory mode, SHALL the C++ core be the only writer of the Alembic
archive for its whole lifetime. The Python Alembic writer SHALL NOT open the
same file during a direct-memory export.

#### Scenario: No mixed writers
- **WHEN** a direct-memory accelerated export runs
- **THEN** only `xgcore.dll` opens and writes the archive

### Requirement: Abort on mid-export read failure

If the direct-memory read fails after the archive is opened, the export SHALL
abort with a clear error message, remove the partial archive, and SHALL NOT
silently switch to the serialized path mid-export.

#### Scenario: Runtime failure in direct-memory mode
- **WHEN** the reader reports a layout or read error after the archive is
  opened
- **THEN** the export stops, the error is shown, and the partial file is
  removed

### Requirement: Version compatibility and read-only lifetime

The direct-memory mode SHALL support the Maya versions whose XGen layouts and
exported symbol sets have been verified (Maya 2019 and 2025), SHALL resolve
XGen member functions by exported symbol name at runtime rather than
hardcoded addresses, SHALL treat the spline data as owned by XGen (read-only
borrow: no deletion, no release, no cross-frame retention), and SHALL read
the data on the Maya main thread during export.

#### Scenario: Maya 2019 and 2025
- **WHEN** the reader runs on Maya 2019 or Maya 2025
- **THEN** the same binary reader works on both, and a documented layout
  re-verification step exists for both versions

#### Scenario: Data ownership
- **WHEN** the reader accesses live spline data
- **THEN** it never deletes or releases the borrowed `XgSplineData` and never
  holds it past the export operation
