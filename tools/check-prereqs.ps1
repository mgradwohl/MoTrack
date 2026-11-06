<#
.SYNOPSIS
Checks the MoTrack developer tools and builds a small Qt and Windows SDK probe.
#>
[CmdletBinding()]
param([switch]$ToolsOnly)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'dev-env.ps1')
$repo = Split-Path -Parent $PSScriptRoot
$missing = @()
foreach ($tool in @('git', 'cmake', 'ninja', 'clang++', 'ccache', 'python', 'clang-format', 'clang-tidy', 'llvm-profdata', 'llvm-cov')) {
    $command = Get-Command "$tool.exe" -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($command) {
        $version = & $command.Source --version | Where-Object { $_ -match 'version|^python |^[0-9]' } | Select-Object -First 1
        if ($LASTEXITCODE -ne 0) { $missing += $tool }
        Write-Host ('{0,-14} {1} [{2}]' -f $tool, $command.Source, $version)
    } else {
        Write-Host ('{0,-14} MISSING' -f $tool)
        $missing += $tool
    }
}
if (-not $env:WindowsSdkDir -or -not $env:VCToolsInstallDir) {
    $missing += 'Visual Studio C++ Build Tools and Windows SDK'
}
if ($missing.Count) {
    throw "Missing prerequisites: $($missing -join ', '). Run tools/setup-dev.ps1."
}
if ($ToolsOnly) { return }
$probe = Join-Path $repo 'build\prereqs'
& cmake --fresh -S (Join-Path $PSScriptRoot 'prereqs') -B $probe -G Ninja -DCMAKE_CXX_COMPILER=clang++
if ($LASTEXITCODE -ne 0) { throw 'Could not configure the Qt and Windows SDK probe.' }
& cmake --build $probe
if ($LASTEXITCODE -ne 0) { throw 'Could not build the Qt and Windows SDK probe.' }
