# Build script for xgcore (Windows, MSVC toolchain via NMake).
#
# Loads the Visual Studio environment into the current process, configures the
# project with CMake (NMake Makefiles generator) and builds with nmake.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File scripts/build.ps1
#
# Parameters (overridable):
#   -VcVars            path to vcvars64.bat (default: this machine's VS layout)
#   -BuildDir          CMake build directory (default: cpp\build)
#   -Config            build type, Release or Debug (default: Release)
#   -AlembicSourceDir  local Alembic source override (default: R:\git\alembic)
#   -ImathSourceDir    local Imath source override
#   -ZlibSourceDir     local zlib source override

param(
  [string]$VcVars = "R:\vs2022\VC\Auxiliary\Build\vcvars64.bat",
  [string]$BuildDir = "cpp\build",
  [string]$Config = "Release",
  [string]$AlembicSourceDir = "R:\git\alembic",
  [string]$ImathSourceDir = "R:\code\PycharmProjects\maya_xgen\XGenUEGroomExporter\cpp\third_party\Imath-3.1.12",
  [string]$ZlibSourceDir = "R:\code\PycharmProjects\maya_xgen\XGenUEGroomExporter\cpp\third_party\zlib-1.3.1"
)

$ErrorActionPreference = "Stop"

function Import-VcVarsEnv {
  param([string]$VcVarsPath)
  if (-not (Test-Path $VcVarsPath)) {
    throw "vcvars64.bat not found at $VcVarsPath"
  }
  # Capture the environment produced by vcvars64.bat and apply it to this
  # PowerShell process so cmake/nmake inherit a normalized environment.
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

& cmake -S cpp -B $BuildDir -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=$Config @sourceArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& cmake --build $BuildDir
exit $LASTEXITCODE
