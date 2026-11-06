# Dot-source this file to use the MoTrack toolchain in the current PowerShell.
param([switch]$NoActivate)

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

function Get-MoTrackPython {
    $candidates = @(
        (Join-Path $env:LOCALAPPDATA 'Programs\Python\Python312\python.exe')
        (Get-Command python.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source)
    )
    foreach ($candidate in $candidates) {
        if ($candidate -and $candidate -notlike '*\Microsoft\WindowsApps\*' -and (Test-Path -LiteralPath $candidate)) {
            & $candidate -c 'import sys; sys.exit(sys.version_info < (3, 10))'
            if ($LASTEXITCODE -eq 0) { return $candidate }
        }
    }
}

function Initialize-MoTrackEnvironment {
    $repo = Split-Path -Parent $PSScriptRoot
    if (-not $env:VCToolsInstallDir -or -not $env:WindowsSdkDir -or $env:VSCMD_ARG_TGT_ARCH -ne 'x64') {
        $vs = Get-MoTrackVisualStudio
        if ($vs) {
            Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll') -ErrorAction Stop
            Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
        }
    }

    $machinePaths = [Environment]::GetEnvironmentVariable('Path', 'Machine') -split ';'
    $userPaths = [Environment]::GetEnvironmentVariable('Path', 'User') -split ';'
    $knownPaths = @(
        (Join-Path $env:ProgramFiles 'LLVM\bin')
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
}

if (-not $NoActivate) { Initialize-MoTrackEnvironment }
