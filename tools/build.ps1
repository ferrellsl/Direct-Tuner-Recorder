# Builds DirectTunerRecorder.exe, and with -Installer the setup program in dist\.
# Needs Visual Studio 2022 with C++, CMake, Ninja, and Inno Setup 6 for the installer.
param([switch]$Installer)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vcvars = Join-Path (& $vswhere -latest -products * -property installationPath) 'VC\Auxiliary\Build\vcvars64.bat'
$build = Join-Path $root 'build'
cmd /c "`"$vcvars`" >nul && cmake -S `"$root`" -B `"$build`" -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build `"$build`""
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if (-not $Installer) { return }

$iscc = "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe"
& $iscc /Q "$root\installer\recorder.iss"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Get-ChildItem "$root\dist\*.exe" | ForEach-Object { "Built $($_.FullName)" }
