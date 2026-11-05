#Requires -Version 5.1
# Download Team-Resurgent/llvm-project rolling "latest" xbox-windows-x64.zip
# into build/llvm (clang, ld.lld, llvm-ar, lib/clang, libcxx/libcxxabi/libunwind).
#
#   powershell -NoProfile -File scripts/fetch-clang.ps1

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path $PSScriptRoot -Parent
Set-Location $RepoRoot

$zipName = "xbox-windows-x64.zip"
New-Item -ItemType Directory -Force -Path deps, build | Out-Null
$zipPath = Join-Path $RepoRoot "deps\$zipName"
if (Test-Path $zipPath) { Remove-Item $zipPath -Force }

if (Get-Command gh -ErrorAction SilentlyContinue) {
    gh release download latest --repo Team-Resurgent/llvm-project `
        --pattern $zipName --dir deps
} else {
    $url = "https://github.com/Team-Resurgent/llvm-project/releases/download/latest/$zipName"
    Write-Host "gh not found; downloading $url"
    Invoke-WebRequest -Uri $url -OutFile $zipPath -UseBasicParsing
}

$zip = Get-ChildItem $zipPath | Select-Object -First 1
if (-not $zip) { throw "$zipName missing after download" }

$tmp = Join-Path $RepoRoot "deps\clang-unpack"
if (Test-Path $tmp) { Remove-Item $tmp -Recurse -Force }
Expand-Archive $zip.FullName -DestinationPath $tmp -Force

$dest = Join-Path $RepoRoot "build\llvm"
if (Test-Path $dest) { Remove-Item $dest -Recurse -Force }
$inner = Get-ChildItem $tmp -Directory | Select-Object -First 1
if ($inner -and (Test-Path (Join-Path $inner.FullName "bin\clang.exe"))) {
    Move-Item $inner.FullName $dest
} elseif (Test-Path (Join-Path $tmp "bin\clang.exe")) {
    Move-Item $tmp $dest
} else {
    throw "unzipped clang payload has no bin/clang.exe"
}

foreach ($p in @("bin\clang.exe", "bin\ld.lld.exe", "bin\llvm-ar.exe", "libcxx\include", "libcxxabi\include", "libunwind\src", "libc\hdr\stdint_proxy.h")) {
    $full = Join-Path $dest $p
    if (-not (Test-Path $full)) { throw "clang zip missing $p" }
}

Write-Host "clang -> $dest"
& (Join-Path $dest "bin\clang.exe") --version
