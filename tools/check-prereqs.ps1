<#
.SYNOPSIS
Checks the MoTrack developer tools against tools/toolchain.env and builds a small Qt, FFmpeg and libarchive probe.
.DESCRIPTION
Activates tools/dev-env.ps1, then reports each tool's path and version against what MoTrack needs:
every LLVM tool (clang, clang++, clang-format, clang-tidy, llvm-cov, llvm-profdata) must be the pinned
LLVM major, and a release other than the one CI installs is a warning; CMake, Ninja, Python, Git, the
MSVC toolset and Qt must meet their floors; ccache is recommended. Each failure prints how to fix it.
Exits 1 when anything required is missing or out of range.
.PARAMETER ToolsOnly
Skips the probe build (and treats a missing Qt as a warning).
#>
[CmdletBinding()]
param([switch]$ToolsOnly)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'dev-env.ps1')
$repo = Split-Path -Parent $PSScriptRoot
$pins = Get-MoTrackToolchain
$LlvmMajor = [int]$pins.MOTRACK_LLVM_MAJOR
$LlvmVersion = $pins.MOTRACK_LLVM_VERSION
$SetupHint = 'Run tools/setup-dev.ps1 (preview with -DryRun), then open a new PowerShell and dot-source tools/dev-env.ps1.'

function New-PrereqRow {
    param([string]$Name, [ValidateSet('ok', 'warn', 'fail')][string]$Status, [string]$Version, [string]$Required, [string]$Detail, [string]$Fix)
    [pscustomobject]@{ Name = $Name; Status = $Status; Version = $Version; Required = $Required; Detail = $Detail; Fix = $Fix }
}

# The first application named -Name on PATH, or $null.
function Find-PrereqTool {
    param([string]$Name)
    Get-Command "$Name.exe" -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty Source
}

# The first capture group of -Pattern in a tool's output, or $null.
function Get-PrereqVersion {
    param([string]$Path, [string]$Pattern, [string[]]$Arguments = @('--version'))
    if (-not $Path) { return $null }
    try { $output = (& $Path @Arguments 2>&1) | Out-String } catch { return $null }
    if ($output -match $Pattern) { return $Matches[1] }
    return $null
}

# An LLVM tool's row: another major (or none) fails, the pinned major at another release warns.
function Get-LlvmToolRow {
    param([string]$Name, [string]$Path, [string]$Release, [int]$Major, [string]$PinnedRelease)
    $required = "= $Major"
    if (-not $Path) {
        return New-PrereqRow $Name fail '' $required '(not on PATH)' "$Name comes with LLVM $PinnedRelease. $SetupHint"
    }
    if (-not $Release) {
        return New-PrereqRow $Name fail 'unknown' $required $Path "$Path --version printed no LLVM version. $SetupHint"
    }
    if ([int]($Release -split '\.')[0] -ne $Major) {
        $fix = "$Path is LLVM $Release; MoTrack formats, analyzes and builds with LLVM $Major only (CI installs $PinnedRelease)."
        if ($Path -like '*\venv\Scripts\*') {
            $fix += " It is the PyPI clang-format older setups installed: python tools/dev_sdk.py install removes it."
        } else {
            $fix += ' Run tools/setup-dev.ps1 -ReplaceLlvm to replace it (preview with -DryRun -ReplaceLlvm), then open a new PowerShell and dot-source tools/dev-env.ps1.'
        }
        return New-PrereqRow $Name fail $Release $required $Path $fix
    }
    if ($Release -ne $PinnedRelease) {
        return New-PrereqRow $Name warn $Release $required $Path "CI installs LLVM $PinnedRelease; this LLVM $Major release can differ from it only by bug fixes."
    }
    return New-PrereqRow $Name ok $Release $required $Path ''
}

# A row for a tool with a floor: missing or older fails, or warns with -Optional.
function Get-MinimumRow {
    param([string]$Name, [string]$Path, [string]$Version, [string]$Minimum, [string]$Fix, [switch]$Optional)
    $bad = if ($Optional) { 'warn' } else { 'fail' }
    $numeric = if ($Version -match '^(\d+(?:\.\d+){1,3})') { $Matches[1] } elseif ($Version -match '^(\d+)') { "$($Matches[1]).0" } else { $null }
    if (-not $numeric) {
        $detail = if ($Path) { $Path } else { '(not found)' }
        return New-PrereqRow $Name $bad $Version ">= $Minimum" $detail $Fix
    }
    if ([version]$numeric -lt [version]$Minimum) {
        return New-PrereqRow $Name $bad $Version ">= $Minimum" $Path $Fix
    }
    return New-PrereqRow $Name ok $Version ">= $Minimum" $Path ''
}

# The MSVC toolset clang uses: the one the developer environment selected, else the newest installed.
function Get-ActiveMsvcToolset {
    $version = $env:VCToolsVersion
    if (-not $version -and $env:VCToolsInstallDir) { $version = Split-Path -Leaf $env:VCToolsInstallDir.TrimEnd('\') }
    $parsed = $null
    if ($version -and [version]::TryParse($version.Trim(), [ref]$parsed)) {
        return [pscustomobject]@{ Version = $parsed; Path = $env:VCToolsInstallDir }
    }
    return $null
}

# The Qt prefix CMake will find first: $env:Qt6_DIR, CMAKE_PREFIX_PATH, then the Qt bin directory on PATH.
function Find-QtPrefix {
    $candidates = @()
    if ($env:Qt6_DIR) { $candidates += $env:Qt6_DIR, (Join-Path $env:Qt6_DIR '..\..\..') }
    if ($env:QT_ROOT_DIR) { $candidates += $env:QT_ROOT_DIR }
    $candidates += @($env:CMAKE_PREFIX_PATH -split ';' | Where-Object { $_ })
    $qtpaths = Find-PrereqTool 'qtpaths'
    if ($qtpaths) { $candidates += Split-Path -Parent (Split-Path -Parent $qtpaths) }
    foreach ($candidate in $candidates) {
        $file = Join-Path $candidate 'lib\cmake\Qt6\Qt6ConfigVersionImpl.cmake'
        if (Test-Path -LiteralPath $file) {
            $version = (Select-String -LiteralPath $file -Pattern 'set\(PACKAGE_VERSION "(\d+\.\d+\.\d+)"\)' | Select-Object -First 1).Matches.Groups[1].Value
            return [pscustomobject]@{ Prefix = [IO.Path]::GetFullPath($candidate); Version = $version }
        }
    }
    return $null
}

function Write-PrereqRow {
    param([object]$Row)
    $mark, $color = switch ($Row.Status) { 'ok' { [char]0x2713, 'Green' } 'warn' { '!', 'Yellow' } default { [char]0x2717, 'Red' } }
    $version = if ($Row.Version) { $Row.Version } else { 'NOT FOUND' }
    Write-Host "$mark " -ForegroundColor $color -NoNewline
    Write-Host $Row.Name.PadRight(15) -NoNewline
    Write-Host "$version ".PadRight(14) -ForegroundColor $color -NoNewline
    Write-Host "($($Row.Required)) ".PadRight(18) -ForegroundColor Cyan -NoNewline
    Write-Host $Row.Detail
}

# Sourced by tools/test-check-prereqs.ps1 to test the functions; the checks run only as a script.
if ($MyInvocation.InvocationName -eq '.') { return }

$rows = [System.Collections.Generic.List[object]]::new()
$llvmPattern = '(?:clang|clang-format|LLVM) version (\d+\.\d+\.\d+)'
foreach ($name in 'clang', 'clang++', 'clang-format', 'clang-tidy', 'llvm-cov', 'llvm-profdata') {
    $path = Find-PrereqTool $name
    $rows.Add((Get-LlvmToolRow -Name $name -Path $path -Release (Get-PrereqVersion $path $llvmPattern) -Major $LlvmMajor -PinnedRelease $LlvmVersion))
}

$msvc = Get-ActiveMsvcToolset
if ($msvc) {
    $rows.Add((Get-MinimumRow -Name 'MSVC toolset' -Path $msvc.Path -Version "$($msvc.Version)" -Minimum $pins.MOTRACK_MSVC_TOOLSET_MIN -Fix "MoTrack builds against Visual Studio 2026's MSVC STL (CI: 14.52). $SetupHint"))
} else {
    $rows.Add((New-PrereqRow 'MSVC toolset' fail '' ">= $($pins.MOTRACK_MSVC_TOOLSET_MIN)" '(no Visual Studio developer environment)' "Install Visual Studio 2026 Build Tools with the C++ tools. $SetupHint"))
}
$sdk = "$env:WindowsSDKVersion".TrimEnd('\')
$sdkPin = (($pins.MOTRACK_WINDOWS_SDK_VERSION -split '\.')[0..2] -join '.') + '.0'
if (-not $sdk -or -not $env:WindowsSdkDir) {
    $rows.Add((New-PrereqRow 'Windows SDK' fail '' ">= $sdkPin" '(not in the developer environment)' "Install the Windows SDK $sdkPin. $SetupHint"))
} else {
    $rows.Add((Get-MinimumRow -Name 'Windows SDK' -Path $env:WindowsSdkDir -Version $sdk -Minimum $sdkPin -Optional -Fix "CI builds with the Windows SDK $sdkPin. $SetupHint"))
}

$cmake = Find-PrereqTool 'cmake'
$rows.Add((Get-MinimumRow -Name 'cmake' -Path $cmake -Version (Get-PrereqVersion $cmake 'cmake version (\d+\.\d+\.\d+)') -Minimum $pins.MOTRACK_CMAKE_MIN -Fix "winget install --id Kitware.CMake --exact --version $($pins.MOTRACK_CMAKE_VERSION), or $SetupHint"))
$ninja = Find-PrereqTool 'ninja'
$rows.Add((Get-MinimumRow -Name 'ninja' -Path $ninja -Version (Get-PrereqVersion $ninja '(?m)^(\d+\.\d+(?:\.\d+)?)') -Minimum $pins.MOTRACK_NINJA_MIN -Fix "winget install --id Ninja-build.Ninja --exact --version $($pins.MOTRACK_NINJA_VERSION), or $SetupHint"))
$ccache = Find-PrereqTool 'ccache'
$rows.Add((Get-MinimumRow -Name 'ccache' -Path $ccache -Version (Get-PrereqVersion $ccache 'ccache version (\d+\.\d+(?:\.\d+)?)') -Minimum $pins.MOTRACK_CCACHE_MIN -Optional -Fix "Recommended for faster rebuilds: winget install --id Ccache.Ccache --exact --version $($pins.MOTRACK_CCACHE_VERSION)"))
$python = Get-MoTrackPython
if (-not $python) { $python = Find-PrereqTool 'python' }
$rows.Add((Get-MinimumRow -Name 'python' -Path $python -Version (Get-PrereqVersion $python 'Python (\d+\.\d+\.\d+)') -Minimum $pins.MOTRACK_PYTHON_VERSION -Fix "Install Python $($pins.MOTRACK_PYTHON_VERSION) or newer (winget install --id Python.Python.$($pins.MOTRACK_PYTHON_VERSION) --exact), or $SetupHint"))
$git = Find-PrereqTool 'git'
$rows.Add((Get-MinimumRow -Name 'git' -Path $git -Version (Get-PrereqVersion $git 'git version (\d+\.\d+\.\d+)') -Minimum $pins.MOTRACK_GIT_MIN -Fix "winget install --id Git.Git --exact, or $SetupHint"))
$qt = Find-QtPrefix
$qtFix = "python tools/dev_sdk.py install puts Qt $($pins.MOTRACK_QT_VERSION) in .dev-tools; or add a Qt $($pins.MOTRACK_QT_MIN)+ prefix to CMAKE_PREFIX_PATH."
$rows.Add((Get-MinimumRow -Name 'Qt' -Path $(if ($qt) { $qt.Prefix }) -Version $(if ($qt) { $qt.Version }) -Minimum $pins.MOTRACK_QT_MIN -Optional:$ToolsOnly -Fix $qtFix))

Write-Host 'Tool           Version       Required          Path'
Write-Host ('-' * 86)
foreach ($row in $rows) { Write-PrereqRow $row }
Write-Host ('-' * 86)
# One line per distinct fix, naming every row it applies to.
foreach ($group in $rows | Where-Object { $_.Status -ne 'ok' -and $_.Fix } | Group-Object Status, Fix) {
    $color = if ($group.Group[0].Status -eq 'fail') { 'Red' } else { 'Yellow' }
    Write-Host "$(($group.Group.Name) -join ', '): $($group.Group[0].Fix)" -ForegroundColor $color
}
if (@($rows | Where-Object Status -eq 'fail').Count) {
    Write-Host 'Some prerequisites are missing or outside the versions MoTrack needs.' -ForegroundColor Red
    exit 1
}
if ($ToolsOnly) {
    Write-Host 'The required tools are installed.' -ForegroundColor Green
    exit 0
}

$probe = Join-Path $repo 'build\prereqs'
& cmake --fresh -S (Join-Path $PSScriptRoot 'prereqs') -B $probe -G Ninja -DCMAKE_CXX_COMPILER=clang++
if ($LASTEXITCODE -ne 0) {
    Write-Host 'Could not configure the Qt, FFmpeg and libarchive probe. python tools/dev_sdk.py install and python tools/media_sdk.py install provide them.' -ForegroundColor Red
    exit 1
}
& cmake --build $probe
if ($LASTEXITCODE -ne 0) {
    Write-Host 'Could not build the Qt and Windows SDK probe; see the compiler output above.' -ForegroundColor Red
    exit 1
}
Write-Host 'The required tools are installed and the probe builds.' -ForegroundColor Green
exit 0
