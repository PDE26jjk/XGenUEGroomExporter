## ADDED Requirements

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
