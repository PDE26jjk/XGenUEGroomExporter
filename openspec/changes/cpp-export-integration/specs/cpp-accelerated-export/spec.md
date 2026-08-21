## Purpose

Lets users of the XGen Groom Exporter optionally route grooming export through
the compiled C++ core for a large speedup, while keeping the original
pure-Python export path as the default for maximum compatibility.

## ADDED Requirements

### Requirement: Optional accelerated export selection

The exporter UIs SHALL expose a per-export option to enable C++-accelerated
export. The option SHALL be off by default and SHALL be selectable only when
the C++ core is available. When the option is off or the core is unavailable,
the exporter SHALL run the original pure-Python export path unchanged.

#### Scenario: Acceleration disabled by default
- **WHEN** a user opens either exporter window
- **THEN** the acceleration option is unchecked, and exporting produces the
  same output as the original pure-Python path

#### Scenario: Acceleration unavailable
- **WHEN** the C++ core cannot be loaded (missing or incompatible DLL)
- **THEN** the acceleration option is disabled in the UI and the export runs
  through the original pure-Python path

### Requirement: Feature parity in accelerated mode

In accelerated mode, the export SHALL honor every option supported by the
pure-Python path, matching each script's existing UI: group name, group id,
guide flag, bake UV, card id, ptex guide id, guide weights, animation, and
spline animation. In XGenUEGroomExporter, this additionally includes the
per-row curve/XGen type selection (xgenType / curveType); the Description
exporter has no such option, and the accelerated path SHALL NOT introduce
options absent from the original script. The resulting Alembic file SHALL
contain the same objects, arbitrary parameters, and per-frame structure as the
pure-Python path.

#### Scenario: Same scene, both paths
- **WHEN** the same scene is exported once with acceleration off and once with
  acceleration on
- **THEN** both files import successfully and expose the same curve counts,
  per-curve vertex counts, arbitrary parameters, and animation structure

#### Scenario: Every option honored
- **WHEN** a user exports with acceleration on while bake UV, guides, group
  ids, card ids, ptex guide ids, and animation are enabled, and the per-row
  curve/XGen type is set in XGenUEGroomExporter
- **THEN** all corresponding attributes are present in the exported Alembic
  file

### Requirement: Single writer per export

In accelerated mode, the entire Alembic archive SHALL be written by the C++
core. The pure-Python Alembic writer SHALL NOT open the same file during an
accelerated export.

#### Scenario: No mixed writers
- **WHEN** an accelerated export runs
- **THEN** only the C++ core opens and writes the archive for the whole export
  lifetime

### Requirement: Clear failure reporting

If the accelerated path fails after an archive is opened, the export SHALL
abort with a clear error message and SHALL NOT leave a partial archive that is
indistinguishable from a successful export.

#### Scenario: Runtime failure in accelerated mode
- **WHEN** the C++ core reports an error during an accelerated export
- **THEN** the export stops, the error message is shown to the user, and the
  partially written file is removed or clearly marked as failed

### Requirement: Python 2.7 and 3.x compatibility

The acceleration wrapper code SHALL work from the Python 2.7 exporters (Maya
2018 and below) and the Python 3.x exporters (Maya 2020 and above) with the
same DLL, embedded in each exporter script.

#### Scenario: Wrapper runs in both Python generations
- **WHEN** an exporter script is run under Python 2.7 and under Python 3.x
- **THEN** both runs load the DLL and call the same entry points

### Requirement: Single-file distribution

The accelerated path SHALL NOT require the user to install any Python module
beyond the exporter script itself. Each exporter SHALL remain a single
self-contained `.py` file that can be pasted into Maya's script editor and run;
`xgcore.dll` is the only additional runtime artifact.

#### Scenario: Paste and run
- **WHEN** a user pastes an exporter script into Maya's script editor, runs it,
  and the DLL is available on a standard search path
- **THEN** the exporter works, including the acceleration checkbox, without any
  extra module installation
