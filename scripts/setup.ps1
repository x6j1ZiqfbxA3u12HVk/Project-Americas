# One-time setup of a clone and of the output folder.
#   .\scripts\setup.ps1 [-GameDir <unpacked game folder>] [-Root <output folder>]
# Enables the repository's git hooks, creates the output folder and, with -GameDir, writes
# americas.ini there. Folders that must stay local belong in .git\info\exclude.
param(
    [string]$GameDir,
    [string]$Root = 'C:\Project Americas'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot

git -C $repo config core.hooksPath .githooks
if ($LASTEXITCODE -ne 0) { throw 'git config failed' }

New-Item -ItemType Directory -Force -Path $Root | Out-Null
if ($GameDir) {
    if (-not (Test-Path -LiteralPath (Join-Path $GameDir 'MicrosoftGame.Config'))) {
        throw "No MicrosoftGame.Config in '$GameDir': point -GameDir at the unpacked game folder."
    }
    $ini = Join-Path $Root 'americas.ini'
    @(
        '# Project Americas settings (see americas.example.ini in the repository).',
        "game_dir = $GameDir"
    ) | Set-Content -LiteralPath $ini -Encoding utf8
    Write-Host "wrote $ini"
}
Write-Host "hooks enabled; output folder: $Root"
