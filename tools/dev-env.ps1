# Dot-source this file to use the MoTrack toolchain in the current PowerShell.
param([switch]$NoActivate)

# The pinned versions in tools/toolchain.env, as a hashtable of KEY = value.
function Get-MoTrackToolchain {
    $pins = @{}
    foreach ($line in Get-Content -LiteralPath (Join-Path $PSScriptRoot 'toolchain.env') -Encoding utf8) {
        if ($line -match '^([A-Z0-9_]+)=(\S*)\s*$') { $pins[$Matches[1]] = $Matches[2] }
    }
    return $pins
}

function Add-MoTrackPath {
    param([string[]]$Directories)
    $entries = @($Directories) + @($env:Path -split ';')
    $env:Path = (($entries | Where-Object { $_ } | Select-Object -Unique) -join ';')
}

function Get-MoTrackVisualStudio {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    }
}

# The first Python that is at least the pinned series (tools/toolchain.env), skipping the Store alias.
function Get-MoTrackPython {
    $series = [version](Get-MoTrackToolchain).MOTRACK_PYTHON_VERSION
    $compact = "$($series.Major)$($series.Minor)"
    $candidates = @(
        (Join-Path $env:LOCALAPPDATA "Programs\Python\Python$compact\python.exe")
        (Join-Path $env:ProgramFiles "Python$compact\python.exe")
        (Get-Command python.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source)
    )
    foreach ($candidate in $candidates) {
        if ($candidate -and $candidate -notlike '*\Microsoft\WindowsApps\*' -and (Test-Path -LiteralPath $candidate)) {
            & $candidate -c "import sys; sys.exit(sys.version_info < ($($series.Major), $($series.Minor)))"
            if ($LASTEXITCODE -eq 0) { return $candidate }
        }
    }
}

function Initialize-MoTrackEnvironment {
    $repo = Split-Path -Parent $PSScriptRoot
    if (-not $env:VCToolsInstallDir -or -not $env:WindowsSdkDir -or $env:VSCMD_ARG_TGT_ARCH -ne 'x64') {
        $vs = Get-MoTrackVisualStudio
        if ($vs) {
            # VsDevCmd runs a bare vswhere.exe, which cmd only finds in its current directory unless the
            # Installer directory is on PATH (it is not when NoDefaultCurrentDirectoryInExePath is set).
            Add-MoTrackPath @((Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer'))
            Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll') -ErrorAction Stop
            Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
        }
    }

    $machinePaths = [Environment]::GetEnvironmentVariable('Path', 'Machine') -split ';'
    $userPaths = [Environment]::GetEnvironmentVariable('Path', 'User') -split ';'
    $llvm = Join-Path $env:ProgramFiles 'LLVM\bin'
    $knownPaths = @(
        (Join-Path $env:ProgramFiles 'CMake\bin')
        (Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Links')
    ) | Where-Object { Test-Path -LiteralPath $_ }
    Add-MoTrackPath (@($knownPaths) + @($env:Path -split ';') + $machinePaths + $userPaths)

    $python = Get-MoTrackPython
    if ($python) {
        Add-MoTrackPath @(Split-Path -Parent $python)
        $sdk = Join-Path $repo 'tools\dev_sdk.py'
        $tools = & $python $sdk tools-root
        if ($LASTEXITCODE -ne 0) { throw 'Could not locate the MoTrack tools directory.' }
        $qt = & $python $sdk qt-prefix
        if ($LASTEXITCODE -ne 0) { throw 'Could not locate the MoTrack Qt SDK.' }
        $venv = Join-Path $tools 'venv\Scripts'
        if (Test-Path -LiteralPath $venv) { Add-MoTrackPath @($venv) }
        if (Test-Path -LiteralPath (Join-Path $qt 'lib\cmake\Qt6\Qt6Config.cmake')) {
            Add-MoTrackPath @((Join-Path $qt 'bin'))
            $prefixes = @($qt) + @($env:CMAKE_PREFIX_PATH -split ';')
            $env:CMAKE_PREFIX_PATH = (($prefixes | Where-Object { $_ } | Select-Object -Unique) -join ';')
        }
        $media = & $python (Join-Path $repo 'tools\media_sdk.py') prefix
        if ($LASTEXITCODE -ne 0) { throw 'Could not locate the MoTrack FFmpeg SDK.' }
        if (Test-Path -LiteralPath (Join-Path $media 'share\ffmpeg\FindFFMPEG.cmake')) {
            $env:MOTRACK_FFMPEG_ROOT = $media
            Add-MoTrackPath @((Join-Path $media 'bin'))
        }
    }
    # Last, so the pinned LLVM's clang-format wins over the PyPI one older setups put in the venv.
    if (Test-Path -LiteralPath $llvm) { Add-MoTrackPath @($llvm) }
}

if (-not $NoActivate) { Initialize-MoTrackEnvironment }
