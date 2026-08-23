## Purpose

Ensures every frame of an animated export has per-strand vertex counts that
match the first frame, so Alembic curve topology stays consistent across
samples and the exported groom imports cleanly into UE.

## ADDED Requirements

### Requirement: Per-frame strand count alignment

In animated exports, the exporter SHALL align each later frame's per-strand
vertex counts to the first frame's per-strand vertex counts using the
documented resampling algorithm, in both the Python export path and the C++
core (serialized and direct-memory routes). The resampled positions SHALL
follow the strand shape of the frame being exported.

#### Scenario: Frame with a different strand CV count
- **WHEN** a later animation frame contains a strand whose vertex count
  differs from the first frame
- **THEN** the frame is exported with the first frame's vertex count for that
  strand and positions resampled to that count

#### Scenario: Identical topology
- **WHEN** a later frame already matches the first frame's per-strand vertex
  counts
- **THEN** positions are exported unchanged and output stays byte-identical
  to today's behavior

### Requirement: Structurally consistent samples

After alignment, every sample in an animated Alembic export SHALL carry the
same curve count, per-curve vertex counts, orders, knots, and widths as the
first sample, with a positions array whose total length equals the declared
vertex counts.

#### Scenario: Read-back validation
- **WHEN** an animated export with changing strand counts is read back
- **THEN** all samples report identical vertex-count arrays and positions
  arrays of matching length

### Requirement: Explicit failure when alignment is impossible

When a strand cannot be aligned to the first frame (for example it disappears
or its topology becomes invalid), the exporter SHALL abort the export with a
clear error message and remove the partial archive, and SHALL NOT write
inconsistent samples.

#### Scenario: Strand disappears
- **WHEN** a later frame is missing a strand that existed in the first frame
  and alignment cannot represent it
- **THEN** the export stops, the error names the affected strand, and no
  partial archive is left
