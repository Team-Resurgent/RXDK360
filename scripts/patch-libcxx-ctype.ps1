#Requires -Version 5.1
# Point libc++'s newlib ctype_base at picolibc's __CTYPE_* mask names.
#
# The 360 does not define picolibc's legacy single-letter ctype macros (_U.._X):
# they collide with XDK identifiers such as float.h's _chgsign(double _X). libc++'s
# <__locale_dir/ctype_base.h> builds its masks from them, so any C++ title using
# <locale>/<iostream> fails to compile until the header is rewritten to use
# picolibc's __CTYPE_* names. The values are identical, so this is ABI-neutral.
#
# This is the same rewrite the installer's PatchLibcxxCtype applies
# (vs20xx/installer/unpacker/Program.cs); fetch-clang.ps1 runs it after staging
# build/llvm. Idempotent -- run it directly to fix an already-staged toolchain:
#
#   powershell -NoProfile -File scripts/patch-libcxx-ctype.ps1

param(
    [string]$LlvmRoot = (Join-Path (Split-Path $PSScriptRoot -Parent) "build\llvm")
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$ctypeBase = Join-Path $LlvmRoot "libcxx\include\__locale_dir\ctype_base.h"
if (-not (Test-Path $ctypeBase)) { throw "libc++ ctype_base.h not found under $LlvmRoot" }

# Each key is a full static_cast<mask>(...) expression whose ')' anchors it, so
# replacements cannot overlap; longest first, matching the installer.
$map = [ordered]@{
    'static_cast<mask>(_P | _U | _L | _N | _B)' = 'static_cast<mask>(__CTYPE_PUNCT | __CTYPE_UPPER | __CTYPE_LOWER | __CTYPE_DIGIT | __CTYPE_BLANK)'
    'static_cast<mask>(_X | _N)'               = 'static_cast<mask>(__CTYPE_HEX | __CTYPE_DIGIT)'
    'static_cast<mask>(_U | _L)'               = 'static_cast<mask>(__CTYPE_UPPER | __CTYPE_LOWER)'
    'static_cast<mask>(_S)'                    = 'static_cast<mask>(__CTYPE_SPACE)'
    'static_cast<mask>(_C)'                    = 'static_cast<mask>(__CTYPE_CNTRL)'
    'static_cast<mask>(_U)'                    = 'static_cast<mask>(__CTYPE_UPPER)'
    'static_cast<mask>(_L)'                    = 'static_cast<mask>(__CTYPE_LOWER)'
    'static_cast<mask>(_N)'                    = 'static_cast<mask>(__CTYPE_DIGIT)'
    'static_cast<mask>(_P)'                    = 'static_cast<mask>(__CTYPE_PUNCT)'
    'static_cast<mask>(_B)'                    = 'static_cast<mask>(__CTYPE_BLANK)'
}

$txt = [IO.File]::ReadAllText($ctypeBase)
$patched = 0
foreach ($k in $map.Keys) {
    if ($txt.Contains($k)) { $txt = $txt.Replace($k, $map[$k]); $patched++ }
}
if ($patched -gt 0) {
    [IO.File]::WriteAllText($ctypeBase, $txt)
    Write-Host "patched libc++ ctype_base -> __CTYPE_* ($patched masks)"
} else {
    Write-Host "libc++ ctype_base already uses __CTYPE_*"
}
