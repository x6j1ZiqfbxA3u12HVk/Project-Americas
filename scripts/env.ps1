# Enters the Visual Studio x64 developer environment (MSVC, CMake, Ninja) in this PowerShell session.
# Usage, from the repository root:  . .\scripts\env.ps1
$ErrorActionPreference = 'Stop'
if ($env:VSCMD_ARG_TGT_ARCH -eq 'x64') { return }

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw 'vswhere.exe not found: install Visual Studio 2022 with "Desktop development with C++".' }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'No Visual Studio installation with the x64 C++ tools was found.' }

# The developer shell calls vswhere by name.
$env:PATH = "$(Split-Path -Parent $vswhere);$env:PATH"

& (Join-Path $vs 'Common7\Tools\Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
