#Requires -Version 5.1
# Initialise the RXDK-360 submodules.
#
#   vendor/picolibc  - the C23 libc (full checkout; Xbox changes live on the
#                      Team-Resurgent/picolibc xbox branch, the consolidated
#                      xboxog+xbox360 branch)
#   samples          - RXDK360-Samples, which the installer ships as
#                      Source\Samples (vs20xx/installer/RXDK-360.iss). Shallow:
#                      only the pinned commit's files are needed.
#
# Clang, lld, libcxx, libcxxabi and libunwind come from the llvm-project
# GitHub zip (scripts/fetch-clang.ps1 -> build/llvm), not a submodule.
# XexTool comes from Team-Resurgent/XexTool latest (scripts/fetch-xextool.ps1).

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path $PSScriptRoot -Parent
Set-Location $RepoRoot

$lp = @('-c', 'core.longpaths=true')

Write-Host 'Initializing picolibc (full checkout)...'
git @lp submodule update --init vendor/picolibc
if ($LASTEXITCODE -ne 0) { throw "submodule update (picolibc) failed ($LASTEXITCODE)" }

Write-Host 'Initializing samples (shallow)...'
git @lp submodule update --init --depth 1 samples
if ($LASTEXITCODE -ne 0) { throw "submodule update (samples) failed ($LASTEXITCODE)" }

Write-Host 'Submodule status:'
git submodule status
