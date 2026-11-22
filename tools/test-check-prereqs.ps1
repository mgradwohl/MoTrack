#Requires -Version 7
# Tests for tools/check-prereqs.ps1, adapted from TaskSmack's tools/test-check-prereqs.ps1. The checker
# is dot-sourced (it returns before its checks then) so its row functions run on chosen inputs; then it
# runs whole with -ToolsOnly from a linked worktree, where it must report the main checkout's Qt, and
# from this checkout, where its table has to agree with its exit code.
# Run with: pwsh -NoProfile -File tools/test-check-prereqs.ps1
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

$checker = Join-Path $PSScriptRoot 'check-prereqs.ps1'
. $checker
$pinned = Get-MoTrackToolchain

# LLVM rows: another major fails, the pinned major at another release warns, the pinned release passes.
foreach ($case in @(
        @{ Path = $null; Release = $null; Status = 'fail'; Fix = '*comes with LLVM 23.1.5*setup-dev.ps1*' }
        @{ Path = 'C:\LLVM\bin\clang.exe'; Release = $null; Status = 'fail'; Fix = '*printed no LLVM version*' }
        @{ Path = 'C:\LLVM\bin\clang.exe'; Release = '22.1.8'; Status = 'fail'; Fix = '*is LLVM 22.1.8; MoTrack formats, analyzes and builds with LLVM 23 only*setup-dev.ps1 -ReplaceLlvm*' }
        @{ Path = 'C:\src\.dev-tools\windows\venv\Scripts\clang-format.exe'; Release = '18.1.8'; Status = 'fail'; Fix = '*PyPI clang-format*dev_sdk.py install removes it.' }
        @{ Path = 'C:\LLVM\bin\clang.exe'; Release = '23.1.6'; Status = 'warn'; Fix = 'CI installs LLVM 23.1.5*' }
        @{ Path = 'C:\LLVM\bin\clang.exe'; Release = '23.1.5'; Status = 'ok'; Fix = '' }
    )) {
    $row = Get-LlvmToolRow -Name clang -Path $case.Path -Release $case.Release -Major 23 -PinnedRelease '23.1.5'
    Assert-True ($row.Status -eq $case.Status -and $row.Required -eq '= 23') "clang $($case.Release) at $($case.Path): $($row.Status) ($($row.Required))"
    Assert-True ($row.Fix -like $case.Fix) "clang $($case.Release) at $($case.Path): fix [$($row.Fix)]"
}

# Floors: missing or older fails (or warns when optional); a bare major parses.
foreach ($case in @(
        @{ Version = $null; Minimum = '3.25'; Optional = $false; Status = 'fail' }
        @{ Version = '3.24.4'; Minimum = '3.25'; Optional = $false; Status = 'fail' }
        @{ Version = '3.25.0'; Minimum = '3.25'; Optional = $false; Status = 'ok' }
        @{ Version = '4.4.4'; Minimum = '3.25'; Optional = $false; Status = 'ok' }
        @{ Version = '4.8.3'; Minimum = '4.9'; Optional = $true; Status = 'warn' }
        @{ Version = '14.52.37025'; Minimum = '14.50'; Optional = $false; Status = 'ok' }
        @{ Version = '14.44.35207'; Minimum = '14.50'; Optional = $false; Status = 'fail' }
        @{ Version = '7'; Minimum = '6.8'; Optional = $false; Status = 'ok' }
        @{ Version = 'garbage'; Minimum = '6.8'; Optional = $true; Status = 'warn' }
    )) {
    $row = Get-MinimumRow -Name tool -Path 'C:\tool.exe' -Version $case.Version -Minimum $case.Minimum -Fix 'fix it' -Optional:$case.Optional
    Assert-True ($row.Status -eq $case.Status -and $row.Required -eq ">= $($case.Minimum)") "$($case.Version) vs $($case.Minimum): $($row.Status)"
    Assert-True (($row.Status -eq 'ok') -eq ($row.Fix -eq '')) "$($case.Version) vs $($case.Minimum): fix [$($row.Fix)]"
}

# The MSVC toolset of the developer environment, from VCToolsVersion or VCToolsInstallDir.
$saved = @{ VCToolsVersion = $env:VCToolsVersion; VCToolsInstallDir = $env:VCToolsInstallDir; Qt6_DIR = $env:Qt6_DIR; QT_ROOT_DIR = $env:QT_ROOT_DIR; CMAKE_PREFIX_PATH = $env:CMAKE_PREFIX_PATH; Path = $env:Path }
$root = Join-Path ([IO.Path]::GetTempPath()) "motrack-check-prereqs-tests-$([guid]::NewGuid().ToString('N'))"
try {
    $env:VCToolsVersion = '14.52.37025'
    $env:VCToolsInstallDir = 'C:\VS\VC\Tools\MSVC\14.52.37025\'
    Assert-True ((Get-ActiveMsvcToolset).Version -eq [version]'14.52.37025') 'MSVC from VCToolsVersion'
    Remove-Item Env:VCToolsVersion
    $env:VCToolsInstallDir = 'C:\VS\VC\Tools\MSVC\14.44.35207\'
    Assert-True ((Get-ActiveMsvcToolset).Version -eq [version]'14.44.35207') 'MSVC from VCToolsInstallDir'
    Remove-Item Env:VCToolsInstallDir
    Assert-True ($null -eq (Get-ActiveMsvcToolset)) 'MSVC without a developer environment'

    # Qt: Qt6_DIR (a prefix or its lib\cmake\Qt6), QT_ROOT_DIR and CMAKE_PREFIX_PATH, in that order.
    $qt = Join-Path $root 'Qt\6.8.3\msvc2022_64'
    $config = Join-Path $qt 'lib\cmake\Qt6'
    New-Item -ItemType Directory -Force -Path $config | Out-Null
    Set-Content -LiteralPath (Join-Path $config 'Qt6ConfigVersionImpl.cmake') -Value 'set(PACKAGE_VERSION "6.8.3")' -Encoding utf8
    Remove-Item Env:Qt6_DIR, Env:QT_ROOT_DIR -ErrorAction SilentlyContinue
    $env:CMAKE_PREFIX_PATH = "C:\missing;$qt"
    $env:Path = "$env:SystemRoot\System32"
    $found = Find-QtPrefix
    Assert-True ($found.Version -eq '6.8.3' -and $found.Prefix -eq [IO.Path]::GetFullPath($qt)) "Qt from CMAKE_PREFIX_PATH: [$found]"
    $env:CMAKE_PREFIX_PATH = ''
    $env:Qt6_DIR = $config
    Assert-True ((Find-QtPrefix).Prefix -eq [IO.Path]::GetFullPath($qt)) 'Qt from Qt6_DIR (lib\cmake\Qt6)'
    Remove-Item Env:Qt6_DIR
    $env:QT_ROOT_DIR = $qt
    Assert-True ((Find-QtPrefix).Prefix -eq [IO.Path]::GetFullPath($qt)) 'Qt from QT_ROOT_DIR'
    Remove-Item Env:QT_ROOT_DIR
    Assert-True ($null -eq (Find-QtPrefix)) 'no Qt'
} finally {
    foreach ($key in $saved.Keys) {
        if ($null -ne $saved[$key]) { Set-Item -LiteralPath "Env:$key" -Value $saved[$key] } else { Remove-Item -LiteralPath "Env:$key" -ErrorAction SilentlyContinue }
    }
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}

$pwsh = (Get-Process -Id $PID).Path

# From a linked worktree without a .dev-tools, the checker reports the main checkout's Qt: a scratch
# repository (git without the user's configuration) whose main checkout has a fake Qt, and a linked
# worktree with a copy of the checker and the tools it runs.
if (-not (Get-Command git -CommandType Application -ErrorAction SilentlyContinue)) {
    Write-Warning 'git is not on PATH; skipping the worktree check.'
} else {
    $work = Join-Path ([IO.Path]::GetTempPath()) "motrack-check-prereqs-worktree-$([guid]::NewGuid().ToString('N'))"
    $names = 'GIT_CONFIG_GLOBAL', 'GIT_CONFIG_NOSYSTEM', 'MOTRACK_DEV_TOOLS', 'CMAKE_PREFIX_PATH', 'Qt6_DIR', 'QT_ROOT_DIR', 'VCToolsInstallDir', 'WindowsSdkDir', 'VSCMD_ARG_TGT_ARCH'
    $savedEnv = @{}
    foreach ($name in $names) { $savedEnv[$name] = [Environment]::GetEnvironmentVariable($name) }
    function Invoke-ScratchGit {
        git @args
        if ($LASTEXITCODE -ne 0) { throw "git $($args -join ' ') exited $LASTEXITCODE" }
    }
    try {
        New-Item -ItemType Directory -Path $work | Out-Null
        $env:GIT_CONFIG_GLOBAL = Join-Path $work 'gitconfig'
        $env:GIT_CONFIG_NOSYSTEM = '1'
        Set-Content -LiteralPath $env:GIT_CONFIG_GLOBAL -Value '' -Encoding utf8
        Remove-Item Env:MOTRACK_DEV_TOOLS, Env:CMAKE_PREFIX_PATH, Env:Qt6_DIR, Env:QT_ROOT_DIR -ErrorAction SilentlyContinue
        # Only the Qt row is under test, so Visual Studio's developer shell is skipped.
        $env:VCToolsInstallDir = 'C:\stub\'
        $env:WindowsSdkDir = 'C:\stub\'
        $env:VSCMD_ARG_TGT_ARCH = 'x64'
        $main = Join-Path $work 'main'
        $linked = Join-Path $work 'linked'
        Invoke-ScratchGit init --quiet $main
        Invoke-ScratchGit -C $main -c user.name=MoTrack -c user.email=motrack@example.invalid commit --quiet --allow-empty -m init
        Invoke-ScratchGit -C $main worktree add --quiet --detach $linked
        $tools = (New-Item -ItemType Directory -Path (Join-Path $linked 'tools')).FullName
        foreach ($file in 'check-prereqs.ps1', 'dev-env.ps1', 'dev_sdk.py', 'media_sdk.py', 'toolchain.env') {
            Copy-Item -LiteralPath (Join-Path $PSScriptRoot $file) -Destination $tools
        }
        $qtVersion = $pinned.MOTRACK_QT_VERSION
        $config = Join-Path $main ".dev-tools\windows\qt\$qtVersion\msvc2022_64\lib\cmake\Qt6"
        New-Item -ItemType Directory -Force -Path $config | Out-Null
        Set-Content -LiteralPath (Join-Path $config 'Qt6Config.cmake') -Value '' -Encoding utf8
        Set-Content -LiteralPath (Join-Path $config 'Qt6ConfigVersionImpl.cmake') -Value "set(PACKAGE_VERSION `"$qtVersion`")" -Encoding utf8
        $output = & $pwsh -NoProfile -File (Join-Path $tools 'check-prereqs.ps1') -ToolsOnly 2>&1 | Out-String
        $qtRows = @($output -split '\r?\n' | Where-Object { $_ -match '^\S Qt\s' })
        Assert-True ($qtRows.Count -eq 1 -and $qtRows[0] -like "* Qt *$qtVersion *\main\.dev-tools\windows\qt\$qtVersion\msvc2022_64") "worktree: Qt row is not the main checkout's:`n$output"
        Assert-True (-not (Test-Path -LiteralPath (Join-Path $linked '.dev-tools'))) 'check-prereqs.ps1 created a .dev-tools in the worktree'
    } finally {
        foreach ($name in $savedEnv.Keys) { [Environment]::SetEnvironmentVariable($name, $savedEnv[$name]) }
        if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
    }
}

# The checker as a script: one row per tool, and exit 1 exactly when a row failed.
$output = & $pwsh -NoProfile -File $checker -ToolsOnly 2>&1 | Out-String
$exit = $LASTEXITCODE
$lines = $output -split '\r?\n'
foreach ($name in 'clang', 'clang\+\+', 'clang-format', 'clang-tidy', 'llvm-cov', 'llvm-profdata', 'MSVC toolset', 'Windows SDK', 'cmake', 'ninja', 'ccache', 'python', 'git', 'Qt') {
    Assert-True (@($lines | Where-Object { $_ -match "^\S $name\s" }).Count -eq 1) "Expected one $name row:`n$output"
}
Assert-True (@($lines | Where-Object { $_ -match '^\S clang\s' -and $_ -match '\(= 23\)' }).Count -eq 1) "clang row lacks the pinned major:`n$output"
Assert-True (@($lines | Where-Object { $_ -match '^\S MSVC toolset\s' -and $_ -match ([regex]::Escape("(>= $($pinned.MOTRACK_MSVC_TOOLSET_MIN))")) }).Count -eq 1) "MSVC row lacks the floor:`n$output"
$failed = @($lines | Where-Object { $_ -match "^$([char]0x2717) " }).Count -gt 0
Assert-True (($exit -ne 0) -eq $failed) "exit $exit with failed rows: $failed`n$output"
Write-Host 'check-prereqs.ps1 tests passed'
