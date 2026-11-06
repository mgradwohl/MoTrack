<#
.SYNOPSIS
Installs the MoTrack developer tools through WinGet and downloads a local Qt SDK.
.PARAMETER DryRun
Shows the planned installations without installing anything.
.PARAMETER Build
Builds and tests MoTrack after setup.
#>
[CmdletBinding()]
param([switch]$DryRun, [switch]$Build)
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'dev-env.ps1') -NoActivate
$repo = Split-Path -Parent $PSScriptRoot
if ($env:PROCESSOR_ARCHITECTURE -ne 'AMD64') { throw 'Automatic Windows setup requires an x64 PowerShell session.' }
if (-not $DryRun -and -not (Get-Command winget.exe -ErrorAction SilentlyContinue)) {
    throw 'Install or update App Installer from the Microsoft Store to get WinGet, then rerun setup.'
}

function Install-MoTrackPackage {
    param([string]$Id, [string[]]$ExtraArguments = @())
    $arguments = @('install', '--id', $Id, '--exact', '--source', 'winget',
        '--accept-package-agreements', '--accept-source-agreements') + $ExtraArguments
    if ($DryRun) {
        Write-Host "[dry-run] winget $($arguments -join ' ')"
        return
    }
    & winget.exe @arguments
    if ($LASTEXITCODE -eq 3010) { throw 'Windows requires a restart. Restart and rerun setup.' }
    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne -1978334963) {
        throw "WinGet failed for $Id with exit code $LASTEXITCODE."
    }
}

if ($DryRun -or -not (Get-MoTrackVisualStudio)) {
    Install-MoTrackPackage 'Microsoft.VisualStudio.2022.BuildTools' @(
        '--override', '--wait --passive --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended')
}
if (-not $DryRun) { Initialize-MoTrackEnvironment }
if ($DryRun -or -not $env:WindowsSdkDir) { Install-MoTrackPackage 'Microsoft.WindowsSDK.10.0.26100' }
if ($DryRun -or -not (Get-Command git.exe -ErrorAction SilentlyContinue)) { Install-MoTrackPackage 'Git.Git' }
if ($DryRun -or -not (Get-Command cmake.exe -ErrorAction SilentlyContinue)) { Install-MoTrackPackage 'Kitware.CMake' }
if ($DryRun -or -not (Get-Command ninja.exe -ErrorAction SilentlyContinue)) { Install-MoTrackPackage 'Ninja-build.Ninja' }
if ($DryRun -or -not (Get-Command clang++.exe -ErrorAction SilentlyContinue)) { Install-MoTrackPackage 'LLVM.LLVM' }
if ($DryRun -or -not (Get-Command ccache.exe -ErrorAction SilentlyContinue)) { Install-MoTrackPackage 'ccache.ccache' }
if ($DryRun -or -not (Get-MoTrackPython)) { Install-MoTrackPackage 'Python.Python.3.12' }

if ($DryRun) {
    Write-Host '[dry-run] Install project-local Qt, FFmpeg, libarchive, and the LLVM 18 formatter, then check prerequisites.'
    if ($Build) { Write-Host '[dry-run] cmake --workflow --preset dev' }
} else {
    Initialize-MoTrackEnvironment
    $python = Get-MoTrackPython
    if (-not $python) { throw 'Python 3.10 or newer is required. Open a new PowerShell and rerun setup.' }
    & $python (Join-Path $PSScriptRoot 'dev_sdk.py') install
    if ($LASTEXITCODE -ne 0) { throw 'MoTrack SDK installation failed.' }
    & $python (Join-Path $PSScriptRoot 'media_sdk.py') install
    if ($LASTEXITCODE -ne 0) { throw 'MoTrack media SDK installation failed.' }
    & (Join-Path $PSScriptRoot 'check-prereqs.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'MoTrack prerequisites check failed.' }
    if ($Build) {
        Push-Location $repo
        try {
            Initialize-MoTrackEnvironment
            & cmake --workflow --preset dev
            if ($LASTEXITCODE -ne 0) { throw 'MoTrack build or tests failed.' }
        } finally { Pop-Location }
    }
}
Write-Host "`nFor subsequent terminals, run from the checkout:"
Write-Host '  . .\tools\dev-env.ps1'
Write-Host '  cmake --workflow --preset dev'
