# Dev-loop build script for xgcore.
#
# Builds into a separate build directory (so the linker never touches a
# locked cpp/build/xgcore.dll) and copies the result to a versioned
# xgcore_dev_<timestamp>.dll in the staging directory. The exporter wrappers
# discover the newest xgcore_dev_*.dll in their search directories and load it
# lazily per export, so a rebuild is picked up without restarting Maya.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File scripts/build_dev.ps1
#
# Parameters (overridable):
#   -VcVars            path to vcvars64.bat (default: this machine's VS layout)
#   -BuildDir          dev CMake build directory (default: cpp\build_dev)
#   -StagingDir        where xgcore_dev_*.dll is copied (default: repo root)
#   -Config            build type, Release or Debug (default: Release)
#   -AlembicSourceDir  local Alembic source override (default: R:\git\alembic)
#   -ImathSourceDir    local Imath source override
#   -ZlibSourceDir     local zlib source override

param(
  [string]$VcVars = "R:\vs2022\VC\Auxiliary\Build\vcvars64.bat",
  [string]$BuildDir = "cpp\build_dev",
  [string]$StagingDir = "",
  [string]$Config = "Release",
  [string]$AlembicSourceDir = "R:\git\alembic",
  [string]$ImathSourceDir = "R:\code\PycharmProjects\maya_xgen\XGenUEGroomExporter\cpp\third_party\Imath-3.1.12",
  [string]$ZlibSourceDir = "R:\code\PycharmProjects\maya_xgen\XGenUEGroomExporter\cpp\third_party\zlib-1.3.1"
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
if (-not $StagingDir) {
  $StagingDir = $repoRoot
}
$BuildDir = (Join-Path $repoRoot $BuildDir)

function Import-VcVarsEnv {
  param([string]$VcVarsPath)
  if (-not (Test-Path $VcVarsPath)) {
    throw "vcvars64.bat not found at $VcVarsPath"
  }
  $envLines = cmd /c "call `"$VcVarsPath`" >nul && set"
  if ($LASTEXITCODE -ne 0) {
    throw "vcvars64.bat failed with exit code $LASTEXITCODE"
  }
  foreach ($line in $envLines) {
    if ($line -match '^([^=]+)=(.*)$') {
      Set-Item -Path "Env:$($matches[1])" -Value $matches[2]
    }
  }
}

Import-VcVarsEnv -VcVarsPath $VcVars

$sourceArgs = @()
if ($AlembicSourceDir) { $sourceArgs += "-DALEMBIC_SOURCE_DIR=$AlembicSourceDir" }
if ($ImathSourceDir) { $sourceArgs += "-DIMATH_SOURCE_DIR=$ImathSourceDir" }
if ($ZlibSourceDir) { $sourceArgs += "-DZLIB_SOURCE_DIR=$ZlibSourceDir" }

& cmake -S (Join-Path $repoRoot "cpp") -B $BuildDir -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=$Config @sourceArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& cmake --build $BuildDir
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$built = Join-Path $BuildDir "xgcore.dll"
if (-not (Test-Path $built)) {
  throw "expected build output not found: $built"
}

$stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$target = Join-Path $StagingDir ("xgcore_dev_{0}.dll" -f $stamp)
Copy-Item -LiteralPath $built -Destination $target -Force
Write-Host "Built and staged dev DLL: $target"
