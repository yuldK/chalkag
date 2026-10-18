[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$project_root = Split-Path -Parent $PSScriptRoot
& (Join-Path $project_root 'third_party/luil/scripts/check_source_style.ps1') -root $project_root
