[CmdletBinding()]
param(
    [switch]$SkipSkia
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$project_root = Split-Path -Parent $PSScriptRoot

Push-Location $project_root
try {
    & git submodule update --init third_party/luil
    if ($LASTEXITCODE -ne 0) {
        throw 'Failed to initialize luil.'
    }
    & git -C third_party/luil submodule update --init third_party/nlohmann_json
    if ($LASTEXITCODE -ne 0) {
        throw 'Failed to initialize nlohmann/json.'
    }
    if (-not $SkipSkia) {
        & ./third_party/luil/scripts/fetch_skia.ps1 -Configuration Debug,Release
        if ($LASTEXITCODE -ne 0) {
            throw 'Failed to prepare Skia.'
        }
    }
}
finally {
    Pop-Location
}
