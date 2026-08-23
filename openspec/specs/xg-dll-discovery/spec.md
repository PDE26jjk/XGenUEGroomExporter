# xg-dll-discovery Specification

## Purpose

Defines how the embedded wrapper locates and loads `xgcore.dll`, including an
explicit development override and per-export reload so a rebuilt DLL is
picked up without restarting Maya.

## Requirements

### Requirement: Explicit DLL override

The wrapper SHALL accept an explicit DLL location from an environment
variable (`XGEN_XGCORE_DLL`) or from a sidecar path file
(`xgcore_dev_path.txt`) placed in a searched directory, each supporting an
absolute path. An explicit override SHALL take precedence over all search
directories. The sidecar file SHALL be re-read at export time so a changed
path is picked up in the same Maya session.

#### Scenario: Environment variable set
- **WHEN** `XGEN_XGCORE_DLL` points to an existing DLL before Maya starts
- **THEN** the wrapper loads exactly that DLL

#### Scenario: Sidecar path changed mid-session
- **WHEN** `xgcore_dev_path.txt` is updated while Maya is running and the
  next export starts
- **THEN** the export loads the DLL at the new path without restarting Maya

#### Scenario: No override
- **WHEN** neither the environment variable nor the sidecar file is present
- **THEN** the existing fixed-name search order is used unchanged

### Requirement: Lazy reload in development

In development mode, the wrapper SHALL re-resolve and reload the DLL before
each export so a rebuilt DLL on disk is used without restarting Maya. In
release mode the wrapper SHALL keep the current behavior of loading once at
script import.

#### Scenario: Rebuilt DLL picked up
- **WHEN** a developer rebuilds the DLL and starts a new export in
  development mode
- **THEN** the new binary is loaded and used without restarting Maya

#### Scenario: Release mode unchanged
- **WHEN** no development override or dev DLL is present
- **THEN** the DLL is loaded once at import time, as today

### Requirement: Versioned development DLL discovery

When no explicit override is set, the wrapper SHALL discover versioned
development DLLs (`xgcore_dev_*.dll`) in the searched directories and load
the newest one. When no dev DLL exists, the wrapper SHALL fall back to the
fixed `xgcore.dll` search.

#### Scenario: Dev DLLs present
- **WHEN** one or more `xgcore_dev_*.dll` files exist in the searched
  directories and no override is set
- **THEN** the newest dev DLL is loaded

#### Scenario: No dev DLLs
- **WHEN** only the fixed `xgcore.dll` search would find a DLL
- **THEN** the wrapper behaves exactly as today

### Requirement: Availability gating unchanged

The "Use C++ acceleration" checkbox SHALL remain enabled only when a DLL
loads successfully, and the pure-Python path SHALL remain the default
whenever no DLL is available.

#### Scenario: DLL unavailable
- **WHEN** no DLL can be loaded by any search path or override
- **THEN** the checkbox is disabled and exports run the pure-Python path

### Requirement: Runtime DLL refresh action

The exporter window SHALL provide a "Refresh DLL" action that re-runs DLL
discovery and loading in the current session (not only at script import),
then updates the "Use C++ acceleration" checkbox state and shows either the
loaded DLL path or a clear error message. The refresh SHALL reuse the
existing discovery order (environment override, sidecar, versioned dev DLLs,
fixed-name search). A refresh SHALL be refused while an export archive is
open, and a failed refresh SHALL leave the pure-Python path as the default
with the checkbox disabled.

#### Scenario: DLL placed after import
- **WHEN** the exporter was imported while no DLL was available and the user
  then places a DLL and clicks "Refresh DLL"
- **THEN** the DLL is loaded, the checkbox becomes enabled, and the loaded
  path is shown without restarting Maya

#### Scenario: Refresh with no DLL
- **WHEN** the user clicks "Refresh DLL" and no DLL can be found or loaded
- **THEN** the checkbox stays disabled, the pure-Python path remains the
  default, and the load error is shown

#### Scenario: Refresh during an export
- **WHEN** the user clicks "Refresh DLL" while an export is running
- **THEN** the refresh is refused and the running export is unaffected
