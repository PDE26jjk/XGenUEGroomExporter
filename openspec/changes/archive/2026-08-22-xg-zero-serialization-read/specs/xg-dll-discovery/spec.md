## Purpose

Defines how the embedded wrapper locates and loads `xgcore.dll`, including an
explicit development override and per-export reload so a rebuilt DLL is
picked up without restarting Maya.

## ADDED Requirements

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
