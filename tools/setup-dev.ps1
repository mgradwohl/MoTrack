<#
.SYNOPSIS
Installs the pinned MoTrack developer toolchain on Windows x64 and downloads the local Qt and media SDKs.
.DESCRIPTION
The versions come from tools/toolchain.env, the same pins CI uses. The Windows SDK 26100, CMake, Ninja,
ccache and Python install through WinGet at their pinned versions. Visual Studio Build Tools is
WinGet's current Visual Studio 2026 (18.x) and Git its current release. A tool that is already present
at or above MoTrack's floor is left alone. LLVM is the SHA-256-verified LLVM-<version>-win64.msi from
LLVM's GitHub release, the same file Windows CI installs, into C:\Program Files\LLVM.
An LLVM of another major in C:\Program Files\LLVM has to be uninstalled first, all of its installer
registrations included, since side-by-side LLVM installers share that directory. Setup does that only
with -ReplaceLlvm, after the new installer has been downloaded and verified. LLVM installed in any
other directory is left alone.
Run it from a normal PowerShell: the installers ask for administrator approval (UAC) themselves.
.PARAMETER DryRun
Prints every install, uninstall and download it would run, and runs none of them.
.PARAMETER Build
Configures, builds and tests MoTrack after setup.
.PARAMETER ReplaceLlvm
Allows setup to uninstall the LLVM installations in C:\Program Files\LLVM that are not the pinned major.
#>
[CmdletBinding()]
param([switch]$DryRun, [switch]$Build, [switch]$ReplaceLlvm)
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'dev-env.ps1') -NoActivate
$repo = Split-Path -Parent $PSScriptRoot
$pins = Get-MoTrackToolchain
$LlvmMajor = [int]$pins.MOTRACK_LLVM_MAJOR
$LlvmVersion = $pins.MOTRACK_LLVM_VERSION
$LlvmSha256 = $pins.MOTRACK_LLVM_WIN64_SHA256
$LlvmDir = Join-Path $env:ProgramFiles 'LLVM'
if ($LlvmVersion -notmatch "^$LlvmMajor\.") { throw "tools/toolchain.env pins LLVM $LlvmVersion, outside major $LlvmMajor." }

# winget exit codes (https://github.com/microsoft/winget-cli/blob/master/doc/windows/package-manager/winget/returnCodes.md)
# that mean the package is already there, so a rerun does not stop on them. PowerShell reads these hex
# literals as the negative Int32 values $LASTEXITCODE carries.
#   0x8A15002B UPDATE_NOT_APPLICABLE, 0x8A150061 PACKAGE_ALREADY_INSTALLED
$WinGetAlreadyInstalledCodes = @(0x8A15002B, 0x8A150061)
# 0x8A15010D INSTALL_ALREADY_INSTALLED: another version is installed. Not fatal; check-prereqs reports it.
$WinGetOtherVersionInstalledCode = 0x8A15010D
# 0x8A150017 NO_MANIFEST_FOUND: the catalog no longer carries the pinned version.
$WinGetNoManifestFoundCode = 0x8A150017
$WinGetInstallSwitches = @('--exact', '--source', 'winget', '--accept-package-agreements', '--accept-source-agreements')

# Run winget. A nonzero exit throws, except the already-installed codes and any in -ReturnCodes, which
# return with the code left in $LASTEXITCODE.
function Invoke-WinGet {
    param(
        [int[]]$ReturnCodes = @(),
        [Parameter(Position = 0, ValueFromRemainingArguments)]
        [string[]]$Arguments
    )
    if ($DryRun) {
        Write-Host "[dry-run] winget $($Arguments -join ' ')"
        return
    }
    winget @Arguments
    $exitCode = $LASTEXITCODE
    if ($exitCode -eq 0 -or $ReturnCodes -contains $exitCode) { return }
    $hexCode = '0x{0:X8}' -f $exitCode
    if ($exitCode -eq 3010) { throw 'Windows requires a restart. Restart and rerun setup.' }
    if ($WinGetAlreadyInstalledCodes -contains $exitCode) {
        Write-Host "  Already installed (winget $hexCode); continuing."
        return
    }
    if ($exitCode -eq $WinGetOtherVersionInstalledCode) {
        Write-Warning "winget $hexCode`: another version is already installed (winget $($Arguments -join ' ')). Run tools/check-prereqs.ps1 to see whether it meets MoTrack's floor."
        return
    }
    throw "winget failed with exit code $hexCode ($exitCode): winget $($Arguments -join ' ')"
}

# Install a winget package at -Version when given. winget's catalog keeps only recent versions; when the
# pin is gone (0x8A150017) this warns and installs the catalog's newest instead, so setup still finishes.
function Install-WinGetPackage {
    param(
        [Parameter(Mandatory)][string]$Id,
        [string]$Version,
        [string[]]$Arguments = @()
    )
    if (-not $Version) {
        Invoke-WinGet install --id $Id @WinGetInstallSwitches @Arguments
        return
    }
    Invoke-WinGet -ReturnCodes $WinGetNoManifestFoundCode install --id $Id --version $Version @WinGetInstallSwitches @Arguments
    if ($DryRun) {
        Write-Host "[dry-run]   (if $Version is no longer in the winget catalog: a warning, then the same install without --version)"
        return
    }
    if ($LASTEXITCODE -ne $WinGetNoManifestFoundCode) { return }
    Write-Warning "winget 0x8A150017: $Id $Version is no longer in the winget catalog; installing its newest version instead. Update tools/toolchain.env to a version the catalog carries: winget show --id $Id --versions"
    Invoke-WinGet install --id $Id @WinGetInstallSwitches @Arguments
}

# The version a tool prints (the first capture group of -Pattern), as [version], or $null when it is
# missing or prints none.
function Get-MoTrackToolVersion {
    param([string]$Command, [string]$Pattern, [string[]]$Arguments = @('--version'))
    $tool = Get-Command "$Command.exe" -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $tool) { return $null }
    $output = (& $tool.Source @Arguments 2>&1) | Out-String
    $parsed = $null
    if ($output -match $Pattern -and [version]::TryParse($Matches[1], [ref]$parsed)) { return $parsed }
    return $null
}

# Install -Id at -Version unless the tool is present at or above -Minimum. Prints what it found.
function Install-MoTrackTool {
    param(
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][string]$Id,
        [string]$Version,
        $Found,
        [string]$Minimum
    )
    if ($Found -and (-not $Minimum -or $Found -ge [version]$Minimum)) {
        $pin = if ($Version -and "$Found" -ne $Version) { " (pinned: $Version)" } else { '' }
        Write-Host "==> $Name $Found is installed$pin; leaving it."
        return
    }
    $reason = if ($Found) { "$Found is older than MoTrack's $Minimum" } else { 'not found' }
    Write-Host "==> Installing $(("$Name $Version").Trim()) ($reason)..."
    Install-WinGetPackage -Id $Id -Version $Version
}

# The newest MSVC toolset of any Visual Studio with the C++ tools, as [version], or $null.
function Get-MoTrackMsvcToolset {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { return $null }
    $best = $null
    foreach ($install in @(& $vswhere -all -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)) {
        $toolsets = Join-Path "$install".Trim() 'VC\Tools\MSVC'
        foreach ($dir in Get-ChildItem -LiteralPath $toolsets -Directory -ErrorAction SilentlyContinue) {
            $parsed = $null
            if ([version]::TryParse($dir.Name, [ref]$parsed) -and (-not $best -or $parsed -gt $best)) { $best = $parsed }
        }
    }
    return $best
}

# The Windows SDK files the build needs that are missing for -SdkVersion (10.0.26100.0).
function Get-MissingWindowsSdkFile {
    param([string]$SdkVersion)
    $kits = $null
    foreach ($key in 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots', 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots') {
        $kits = (Get-ItemProperty -LiteralPath $key -Name KitsRoot10 -ErrorAction SilentlyContinue).KitsRoot10
        if ($kits) { break }
    }
    if (-not $kits) { $kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10' }
    @("Include\$SdkVersion\um\Windows.h", "Include\$SdkVersion\ucrt\stdio.h", "Lib\$SdkVersion\um\x64\kernel32.lib", "Lib\$SdkVersion\ucrt\x64\ucrt.lib") |
        ForEach-Object { Join-Path $kits $_ } | Where-Object { -not (Test-Path -LiteralPath $_) }
}

# -- LLVM: exactly one install, of the pinned major --------------------------------------------------
# LLVM's MSIs install side by side, each registered with InstallLocation C:\Program Files\LLVM, and
# older majors used an NSIS installer: the last one installed wins on disk, and uninstalling one can
# remove files the others share. So when anything there is another major, every registration in that
# directory is uninstalled before the pinned MSI is installed (TaskSmack's approach, #3041 there), with
# -ReplaceLlvm and only once the new MSI has been downloaded and verified.

$LlvmUninstallKeys = @(
    'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall',
    'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall',
    'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall'
)

# A directory path in one spelling (environment variables expanded, no trailing separator), or $null.
function ConvertTo-LlvmDirectory {
    param([string]$Path)
    if (-not $Path) { return $null }
    try { return [IO.Path]::GetFullPath([Environment]::ExpandEnvironmentVariables($Path.Trim())).TrimEnd('\') }
    catch { return $null }
}

# The directory a registration installed into: its InstallLocation, else the directory of the uninstaller
# its UninstallString runs. LLVM's NSIS installers (before LLVM 23) write no InstallLocation; their
# uninstaller is <directory>\Uninstall.exe. $null when neither says, as for an MSI without InstallLocation.
function Get-LlvmRegistrationDirectory {
    param([string]$InstallLocation, [string]$UninstallString)
    if ($InstallLocation) { return ConvertTo-LlvmDirectory $InstallLocation }
    $exe = if ($UninstallString -match '^\s*"([^"]+)"') { $Matches[1] } elseif ($UninstallString -match '^\s*(.+?\.exe)(\s|$)') { $Matches[1] } else { $null }
    if (-not $exe -or (Split-Path -Leaf $exe) -ieq 'msiexec.exe') { return $null }
    return ConvertTo-LlvmDirectory (Split-Path -Parent $exe)
}

# Every installer registration of LLVM in the uninstall keys, wherever it is installed (Location; $null
# when the registration does not say).
function Get-InstalledLlvm {
    foreach ($root in $LlvmUninstallKeys) {
        foreach ($key in @(Get-ChildItem -LiteralPath $root -ErrorAction SilentlyContinue)) {
            $item = Get-ItemProperty -LiteralPath $key.PSPath -ErrorAction SilentlyContinue
            if (-not $item) { continue }
            $properties = $item.PSObject.Properties
            $value = { param($Name) if ($properties[$Name]) { "$($properties[$Name].Value)".Trim() } else { '' } }
            if ((& $value 'DisplayName') -notmatch '^LLVM(\s+v?\d[\d.]*)?$') { continue }
            $version = & $value 'DisplayVersion'
            $isMsi = (& $value 'WindowsInstaller') -eq '1' -and $key.PSChildName -match '^\{[0-9A-Fa-f-]{36}\}$'
            [pscustomobject]@{
                Version              = $version
                Major                = if ($version -match '^(\d+)') { [int]$Matches[1] } else { $null }
                ProductCode          = if ($isMsi) { $key.PSChildName } else { $null }
                Location             = Get-LlvmRegistrationDirectory -InstallLocation (& $value 'InstallLocation') -UninstallString (& $value 'UninstallString')
                UninstallString      = & $value 'UninstallString'
                QuietUninstallString = & $value 'QuietUninstallString'
                Key                  = $key.PSPath
            }
        }
    }
}

# The release of $LlvmDir\bin\clang.exe (23.1.5), or $null when it is missing or prints none.
function Get-LlvmClangRelease {
    $clang = Join-Path $LlvmDir 'bin\clang.exe'
    if (-not (Test-Path -LiteralPath $clang)) { return $null }
    $output = (& $clang --version 2>$null) | Out-String
    if ($output -match 'clang version (\d+\.\d+\.\d+)') { return $Matches[1] }
    return $null
}

# What the LLVM step does: @{ Action = Install | Keep | Replace; Remove = registrations; Note }.
#   Install  nothing is registered (an unregistered other major on disk is overwritten).
#   Keep     every registration and clang.exe are the pinned major, whatever the minor.
#   Replace  something is another major or unreadable: uninstall every registration, then install.
function Get-LlvmInstallPlan {
    param(
        [object[]]$Installed = @(),
        [Parameter(Mandatory)][int]$PinnedMajor,
        [Nullable[int]]$ClangMajor
    )
    $Installed = @($Installed)
    if ($Installed.Count -eq 0) {
        if ($ClangMajor -eq $PinnedMajor) {
            return [pscustomobject]@{ Action = 'Keep'; Remove = @(); Note = "LLVM $PinnedMajor is in $LlvmDir without an installer registration." }
        }
        $note = if ($null -ne $ClangMajor) { "LLVM $ClangMajor is in $LlvmDir without an installer registration; installing LLVM $PinnedMajor over it." } else { $null }
        return [pscustomobject]@{ Action = 'Install'; Remove = @(); Note = $note }
    }
    $versions = ($Installed | ForEach-Object { if ($_.Version) { $_.Version } else { '(no version)' } }) -join ', '
    $otherMajors = @($Installed | Where-Object { $_.Major -ne $PinnedMajor })
    if ($otherMajors.Count -eq 0 -and $ClangMajor -eq $PinnedMajor) {
        $note = if ($Installed.Count -gt 1) { "$($Installed.Count) installer registrations of LLVM $PinnedMajor ($versions) share $LlvmDir; leaving them." } else { $null }
        return [pscustomobject]@{ Action = 'Keep'; Remove = @(); Note = $note }
    }
    $reasons = @()
    if ($otherMajors.Count -gt 0) {
        $reasons += "LLVM $(($otherMajors | ForEach-Object { if ($_.Version) { $_.Version } else { '(no version)' } }) -join ', ') is registered"
    }
    if ($ClangMajor -ne $PinnedMajor) {
        $reasons += if ($null -eq $ClangMajor) { "there's no clang.exe in $LlvmDir\bin" } else { "$LlvmDir\bin\clang.exe is LLVM $ClangMajor" }
    }
    return [pscustomobject]@{
        Action = 'Replace'
        Remove = $Installed
        Note   = "$($reasons -join ' and '), but MoTrack builds with LLVM $PinnedMajor; uninstalling every LLVM registration ($versions), then installing LLVM $PinnedMajor."
    }
}

# The quiet uninstall command for one registration: msiexec /x for an MSI, else its
# QuietUninstallString, else its UninstallString with NSIS's /S.
function Get-LlvmUninstallCommand {
    param([Parameter(Mandatory)][object]$Registration)
    if ($Registration.ProductCode) {
        return @{ FilePath = 'msiexec.exe'; ArgumentList = "/x $($Registration.ProductCode) /passive /norestart" }
    }
    $line = $Registration.QuietUninstallString
    $silent = ''
    if (-not $line) {
        $line = $Registration.UninstallString
        $silent = '/S'
    }
    if ($line -match '^\s*"([^"]+)"\s*(.*)$' -or $line -match '^\s*(.+?\.exe)(?:\s+(.*))?$') {
        return @{ FilePath = $Matches[1]; ArgumentList = ("$($Matches[2]) $silent").Trim() }
    }
    throw "LLVM $($Registration.Version) has no usable uninstall command in $($Registration.Key). Uninstall it from Settings > Apps, then rerun this script."
}

# Uninstall one registration (or print the command under -DryRun) and wait until it is gone: an NSIS
# uninstaller hands off to a copy of itself and exits at once.
function Invoke-LlvmUninstall {
    param([Parameter(Mandatory)][object]$Registration, [int]$TimeoutSeconds = 600)
    $command = Get-LlvmUninstallCommand -Registration $Registration
    $shown = "$($command.FilePath) $($command.ArgumentList)".Trim()
    if ($DryRun) {
        Write-Host "[dry-run] $shown"
        return
    }
    Write-Host "  Uninstalling LLVM $($Registration.Version): $shown"
    $retry = 'Uninstall LLVM from Settings > Apps, then rerun this script.'
    $start = @{ FilePath = $command.FilePath; Wait = $true; PassThru = $true }
    if ($command.ArgumentList) { $start.ArgumentList = $command.ArgumentList }
    try { $process = Start-Process @start }
    catch { throw "Uninstalling LLVM $($Registration.Version) didn't start ($($_.Exception.Message)): $shown. $retry" }
    # msiexec: 1605 the product is already gone; 1641 and 3010 a restart finishes it.
    $exitCode = $process.ExitCode
    if (@(0, 1605, 1641, 3010) -notcontains $exitCode -or ($exitCode -ne 0 -and -not $Registration.ProductCode)) {
        throw "Uninstalling LLVM $($Registration.Version) failed with exit code $exitCode`: $shown. $retry"
    }
    if ($exitCode -in 1641, 3010) { Write-Warning "Windows needs a restart to finish uninstalling LLVM $($Registration.Version) (msiexec $exitCode)." }
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while (Test-Path -LiteralPath $Registration.Key) {
        if ((Get-Date) -ge $deadline) { throw "LLVM $($Registration.Version) is still registered $TimeoutSeconds s after its uninstaller exited ($shown). $retry" }
        Start-Sleep -Seconds 2
    }
}

# Download LLVM-<version>-win64.msi from LLVM's GitHub release and check its pinned SHA-256; returns its
# path. Runs before anything is uninstalled, so an outage or a changed file leaves the current LLVM.
function Get-LlvmMsi {
    $name = "LLVM-$LlvmVersion-win64.msi"
    $url = "https://github.com/llvm/llvm-project/releases/download/llvmorg-$LlvmVersion/$name"
    $directory = Join-Path ([IO.Path]::GetTempPath()) 'motrack-setup'
    $installer = Join-Path $directory $name
    if ($DryRun) {
        Write-Host "[dry-run] download $url (SHA-256 $LlvmSha256)"
        return $installer
    }
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
    Write-Host "  Downloading $url"
    Invoke-WebRequest -Uri $url -OutFile $installer -MaximumRetryCount 3 -RetryIntervalSec 10
    $actual = (Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $LlvmSha256) {
        Remove-Item -LiteralPath $installer -Force -ErrorAction SilentlyContinue
        throw "$name has SHA-256 $actual, but tools/toolchain.env pins $LlvmSha256. Nothing was uninstalled or installed."
    }
    return $installer
}

# Install the verified MSI into its default C:\Program Files\LLVM. msiexec /passive asks for
# administrator approval itself.
function Install-LlvmMsi {
    param([Parameter(Mandatory)][string]$Installer)
    $name = Split-Path -Leaf $Installer
    $log = Join-Path (Split-Path -Parent $Installer) "llvm-$LlvmVersion-msi.log"
    $arguments = "/i `"$Installer`" /passive /norestart /l*v `"$log`""
    if ($DryRun) {
        Write-Host "[dry-run] msiexec.exe $arguments"
        return
    }
    $process = Start-Process -FilePath msiexec.exe -ArgumentList $arguments -Wait -PassThru
    if (@(0, 1641, 3010) -notcontains $process.ExitCode) {
        Get-Content -LiteralPath $log -Tail 40 -ErrorAction SilentlyContinue
        throw "Installing $name failed with msiexec exit code $($process.ExitCode) (log: $log)."
    }
    if ($process.ExitCode -ne 0) { Write-Warning "Windows needs a restart to finish installing LLVM $LlvmVersion (msiexec $($process.ExitCode))." }
    Remove-Item -LiteralPath $Installer -Force -ErrorAction SilentlyContinue
}

# The LLVM step: leave exactly one LLVM, of the pinned major, in $LlvmDir, or stop with an error.
function Install-PinnedLlvm {
    [CmdletBinding()]
    param()
    $byVersion = { $parsed = $null; if ([version]::TryParse($_.Version, [ref]$parsed)) { $parsed } else { [version]'0.0' } }
    $registrations = @(Get-InstalledLlvm | Sort-Object -Property Major, $byVersion)
    $directory = ConvertTo-LlvmDirectory $LlvmDir
    $release = Get-LlvmClangRelease
    $clangMajor = if ($release -match '^(\d+)\.') { [int]$Matches[1] } else { $null }
    foreach ($registration in $registrations) {
        $kind = if ($registration.ProductCode) { "MSI $($registration.ProductCode)" } else { 'NSIS .exe' }
        $where = if ($registration.Location) { $registration.Location } else { 'an unrecorded directory' }
        $note = if ($registration.Location -ieq $directory) { '' } else { "; leaving it (MoTrack uses only $LlvmDir)" }
        Write-Host "  Registered: LLVM $($registration.Version) ($kind) in $where$note"
    }
    $installed = @($registrations | Where-Object { $_.Location -and $_.Location -ieq $directory })
    if ($release) { Write-Host "  $LlvmDir\bin\clang.exe is LLVM $release" }
    $plan = Get-LlvmInstallPlan -Installed $installed -PinnedMajor $LlvmMajor -ClangMajor $clangMajor
    if ($plan.Note) { Write-Host "  $($plan.Note)" }
    if ($plan.Action -eq 'Keep') {
        $pin = if ($release -and $release -ne $LlvmVersion) { " CI installs $LlvmVersion; another LLVM $LlvmMajor release formats and analyzes the same way except for fixed bugs." } else { '' }
        Write-Host "  LLVM $LlvmMajor is already installed; leaving it.$pin"
        return
    }
    $remove = @($plan.Remove)
    if ($remove.Count -and -not $ReplaceLlvm) {
        $list = ($remove | ForEach-Object { "LLVM $(if ($_.Version) { $_.Version } else { '(no version)' })" }) -join ', '
        $message = "Installing LLVM $LlvmVersion in $LlvmDir first means uninstalling $list there. Rerun with -ReplaceLlvm to let setup do that (preview it with -DryRun -ReplaceLlvm), or uninstall them from Settings > Apps."
        if ($DryRun) {
            Write-Host "[dry-run] stop: $message"
            return
        }
        throw $message
    }
    $installer = Get-LlvmMsi
    foreach ($registration in $remove) { Invoke-LlvmUninstall -Registration $registration }
    Install-LlvmMsi -Installer $installer
    $check = "$LlvmDir\bin\clang.exe --version"
    if ($DryRun) {
        Write-Host "[dry-run] $check (must report LLVM $LlvmVersion)"
        return
    }
    $after = Get-LlvmClangRelease
    if ($after -ne $LlvmVersion) {
        $found = if ($after) { "LLVM $after" } else { 'no LLVM version (missing, or it does not run)' }
        throw "After installing LLVM $LlvmVersion, $check reports $found. Uninstall every LLVM from Settings > Apps, then rerun this script."
    }
    Write-Host "  $LlvmDir\bin\clang.exe is LLVM $after."
}

# Sourced by tools/test-setup-dev.ps1 through the parser; the steps below run only as a script.
if ($MyInvocation.InvocationName -eq '.') { return }

if ($env:PROCESSOR_ARCHITECTURE -ne 'AMD64') { throw 'Automatic Windows setup requires an x64 PowerShell session.' }
if (-not $DryRun -and -not (Get-Command winget.exe -ErrorAction SilentlyContinue)) {
    throw 'Install or update App Installer from the Microsoft Store to get WinGet, then rerun setup.'
}

Write-Host '=== MoTrack developer setup (Windows) ==='
Write-Host "LLVM $LlvmVersion, CMake $($pins.MOTRACK_CMAKE_VERSION), Ninja $($pins.MOTRACK_NINJA_VERSION), ccache $($pins.MOTRACK_CCACHE_VERSION), Python $($pins.MOTRACK_PYTHON_WINDOWS_VERSION), Qt $($pins.MOTRACK_QT_VERSION)"
if ($DryRun) { Write-Host '(dry-run: nothing is installed)' }

# clang targets the MSVC ABI: it compiles against the MSVC STL and CRT and the Windows SDK it finds.
$msvc = Get-MoTrackMsvcToolset
if ($msvc -and $msvc -ge [version]$pins.MOTRACK_MSVC_TOOLSET_MIN) {
    Write-Host "==> MSVC toolset $msvc is installed; leaving Visual Studio as it is."
} else {
    $reason = if ($msvc) { "the newest MSVC toolset, $msvc, is older than $($pins.MOTRACK_MSVC_TOOLSET_MIN)" } else { 'no Visual Studio C++ toolset found' }
    Write-Host "==> Installing Visual Studio 2026 Build Tools ($reason)..."
    Install-WinGetPackage -Id $pins.MOTRACK_VISUAL_STUDIO_PACKAGE -Arguments @(
        '--override', '--wait --passive --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended')
}

$sdkBuild = ($pins.MOTRACK_WINDOWS_SDK_VERSION -split '\.')[0..2] -join '.'
$missingSdk = @(Get-MissingWindowsSdkFile -SdkVersion "$sdkBuild.0")
if ($missingSdk.Count -eq 0) {
    Write-Host "==> Windows SDK $sdkBuild is installed; leaving it."
} else {
    Write-Host "==> Installing Windows SDK $($pins.MOTRACK_WINDOWS_SDK_VERSION) (desktop C++ x64 headers and libraries)..."
    # --override replaces the manifest's quiet switches, so it carries them itself.
    Install-WinGetPackage -Id "Microsoft.WindowsSDK.$sdkBuild" -Version $pins.MOTRACK_WINDOWS_SDK_VERSION -Arguments @(
        '--override', '/features OptionId.DesktopCPPx64 /quiet /norestart /ceip off')
}

$git = Get-MoTrackToolVersion git 'git version (\d+\.\d+(\.\d+)?)'
Install-MoTrackTool -Name Git -Id Git.Git -Found $git -Minimum $pins.MOTRACK_GIT_MIN
$cmake = Get-MoTrackToolVersion cmake 'cmake version (\d+\.\d+(\.\d+)?)'
Install-MoTrackTool -Name CMake -Id Kitware.CMake -Version $pins.MOTRACK_CMAKE_VERSION -Found $cmake -Minimum $pins.MOTRACK_CMAKE_MIN
$ninja = Get-MoTrackToolVersion ninja '^(\d+\.\d+(\.\d+)?)'
Install-MoTrackTool -Name Ninja -Id Ninja-build.Ninja -Version $pins.MOTRACK_NINJA_VERSION -Found $ninja -Minimum $pins.MOTRACK_NINJA_MIN
$ccache = Get-MoTrackToolVersion ccache 'ccache version (\d+\.\d+(\.\d+)?)'
Install-MoTrackTool -Name ccache -Id Ccache.Ccache -Version $pins.MOTRACK_CCACHE_VERSION -Found $ccache -Minimum $pins.MOTRACK_CCACHE_MIN
$python = Get-MoTrackPython
$pythonVersion = if ($python) { [version]((& $python -c 'import platform; print(platform.python_version())') -replace '[^0-9.].*$', '') } else { $null }
$pythonSeries = [version]$pins.MOTRACK_PYTHON_VERSION
Install-MoTrackTool -Name Python -Id "Python.Python.$($pythonSeries.Major).$($pythonSeries.Minor)" -Version $pins.MOTRACK_PYTHON_WINDOWS_VERSION -Found $pythonVersion -Minimum $pins.MOTRACK_PYTHON_VERSION

Write-Host "==> LLVM $LlvmVersion (one LLVM $LlvmMajor in $LlvmDir)..."
Install-PinnedLlvm

if ($DryRun) {
    Write-Host "[dry-run] python tools/dev_sdk.py install (Qt $($pins.MOTRACK_QT_VERSION) with Qt Multimedia through aqtinstall, or verified archives from download.qt.io)"
    Write-Host '[dry-run] python tools/media_sdk.py install (FFmpeg and libarchive through the pinned vcpkg)'
    Write-Host '[dry-run] tools/check-prereqs.ps1'
    if ($Build) { Write-Host '[dry-run] cmake --workflow --preset dev' }
} else {
    Initialize-MoTrackEnvironment
    $python = Get-MoTrackPython
    if (-not $python) { throw "Python $($pins.MOTRACK_PYTHON_VERSION) or newer is required. Open a new PowerShell and rerun setup." }
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
