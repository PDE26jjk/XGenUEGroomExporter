# exporter-export-hygiene Specification

## Purpose

Controls how the exporter manages temporary scene data and undo behavior
during export, so long animated exports do not accumulate per-frame spline
nodes or unbounded undo records in the Maya session.

## Requirements

### Requirement: Temporary spline cleanup during animated export

During animated exports, the exporter SHALL remove each temporary spline
description node after its frame data has been written, and SHALL remove all
remaining temporary nodes created by the export before the export call
returns, both on success and on failure. Cleanup SHALL only affect nodes
created by the export, never user scene data.

#### Scenario: Long animated export
- **WHEN** an animated export converts the description to interactive splines
  on every frame
- **THEN** each frame's temporary spline node is deleted after that frame is
  written, and none remain in the scene after the export

#### Scenario: Export fails mid-run
- **WHEN** an export raises an error after temporary nodes were created
- **THEN** the temporary nodes created by the export are removed before the
  error propagates

### Requirement: Undo disabled during export

The exporter SHALL disable scene undo for the duration of an export and SHALL
restore the previous undo state when the export finishes, whether it
succeeded or failed.

#### Scenario: Animated export with undo enabled
- **WHEN** a user runs an animated export with undo previously enabled
- **THEN** the conversion calls during the export do not create undo records,
  and undo is restored to its previous enabled state afterwards

### Requirement: Keep temporary data option

The exporter SHALL provide a "Keep temporary data" option, disabled by
default, that when enabled leaves the temporary spline nodes and undo
behavior unchanged so the intermediate scene state can be inspected. When
the option is off (default), temporary cleanup and the disabled-undo scope
SHALL apply.

#### Scenario: Debugging an export
- **WHEN** the user enables "Keep temporary data" before an animated export
- **THEN** temporary nodes remain in the scene after the export and undo
  records are created as before

#### Scenario: Default export
- **WHEN** the user does not enable "Keep temporary data"
- **THEN** cleanup and disabled-undo behavior apply automatically
