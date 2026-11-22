#Requires -Version 7
# Tests for tools/setup-dev.ps1, adapted from TaskSmack's tools/test-setup-dev.ps1. The script is
# dot-sourced (it returns before its install steps then) and its functions run against stub winget,
# Start-Process, Invoke-WebRequest and installed-LLVM readers that record their calls; nothing is
# installed, uninstalled or downloaded. The registry reader runs against a scratch key under HKCU that
# the test deletes. Then tools/dev-env.ps1, which setup sources, activates fake SDKs from a scratch
# repository and its linked worktree. Last, the whole script runs with -DryRun and a stub winget.cmd
# first on PATH.
# Run with: pwsh -NoProfile -File tools/test-setup-dev.ps1
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

# A -like pattern matching Text literally except at its * placeholders ([dry-run] is otherwise a set).
function ConvertTo-Pattern {
    param([string]$Text)
    ($Text -split '\*' | ForEach-Object { [WildcardPattern]::Escape($_) }) -join '*'
}

$setup = Join-Path $PSScriptRoot 'setup-dev.ps1'
. $setup
$pinned = Get-MoTrackToolchain
Assert-True ($LlvmVersion -eq $pinned.MOTRACK_LLVM_VERSION -and $LlvmMajor -eq [int]$pinned.MOTRACK_LLVM_MAJOR) "pins: LLVM $LlvmVersion / $LlvmMajor"
Assert-True ($LlvmSha256 -match '^[0-9a-f]{64}$') "pins: LLVM SHA-256 [$LlvmSha256]"
$DryRun = $false

# The stub winget records each call and answers with $script:Stub's exit code and output.
$script:Calls = [System.Collections.Generic.List[string]]::new()
$script:Stub = { param($Line) @{ ExitCode = 0; Output = @() } }
function winget {
    param([Parameter(ValueFromRemainingArguments)][string[]]$CommandLine)
    $line = $CommandLine -join ' '
    $script:Calls.Add($line)
    $answer = & $script:Stub $line
    $answer.Output
    $global:LASTEXITCODE = $answer.ExitCode
}
function Set-Stub {
    param([scriptblock]$Stub)
    $script:Calls.Clear()
    $script:Stub = $Stub
}
$calls = $script:Calls
$switches = '--exact --source winget --accept-package-agreements --accept-source-agreements'
$pinnedCMake = "install --id Kitware.CMake --version 3.31.12 $switches"
$newestCMake = "install --id Kitware.CMake $switches"
$noManifest = 0x8A150017

# -- winget installs --------------------------------------------------------------------------------
Set-Stub { param($Line) @{ ExitCode = 0; Output = @() } }
$warnings = Install-WinGetPackage -Id Kitware.CMake -Version 3.31.12 3>&1
Assert-True ($calls.Count -eq 1 -and $calls[0] -eq $pinnedCMake) "pin present: calls [$($calls -join '; ')]"
Assert-True (@($warnings).Count -eq 0) "pin present: warned [$warnings]"

Set-Stub { param($Line) if ($Line -like '*--version 3.31.12*') { @{ ExitCode = $noManifest; Output = @() } } else { @{ ExitCode = 0; Output = @() } } }
$warnings = @(Install-WinGetPackage -Id Kitware.CMake -Version 3.31.12 3>&1)
Assert-True ($calls.Count -eq 2 -and $calls[1] -eq $newestCMake) "pin gone: calls [$($calls -join '; ')]"
Assert-True ($warnings.Count -eq 1 -and "$($warnings[0])" -like '*0x8A150017*Kitware.CMake 3.31.12*toolchain.env*') "pin gone: warnings [$warnings]"

Set-Stub { param($Line) @{ ExitCode = 0x8A150011; Output = @() } }
$thrown = $null
try { Install-WinGetPackage -Id Kitware.CMake -Version 3.31.12 } catch { $thrown = $_.Exception.Message }
Assert-True ($thrown -like "winget failed with exit code 0x8A150011 (*): winget $pinnedCMake") "other failure: [$thrown]"
Assert-True ($calls.Count -eq 1) "other failure: calls [$($calls -join '; ')]"

foreach ($case in @(@{ Code = 0x8A15002B; Warnings = 0 }, @{ Code = 0x8A150061; Warnings = 0 }, @{ Code = 0x8A15010D; Warnings = 1 })) {
    Set-Stub { param($Line) @{ ExitCode = $case.Code; Output = @() } }.GetNewClosure()
    $warnings = @(Install-WinGetPackage -Id Kitware.CMake -Version 3.31.12 3>&1 6>$null)
    Assert-True ($calls.Count -eq 1) ("0x{0:X8}: calls [{1}]" -f $case.Code, ($calls -join '; '))
    Assert-True ($warnings.Count -eq $case.Warnings) ("0x{0:X8}: warnings [{1}]" -f $case.Code, ($warnings -join '; '))
}

$DryRun = $true
Set-Stub { param($Line) throw "winget ran under -DryRun: $Line" }
$lines = @(Install-WinGetPackage -Id Kitware.CMake -Version 3.31.12 6>&1 | ForEach-Object { "$_" })
$DryRun = $false
Assert-True ($calls.Count -eq 0 -and $lines.Count -eq 2 -and $lines[0] -eq "[dry-run] winget $pinnedCMake" -and $lines[1] -like '*no longer in the winget catalog*') "dry-run: [$($lines -join '; ')]"

# A present tool at or above the floor is left alone; an older or missing one is installed at the pin.
foreach ($case in @(
        @{ Found = [version]'4.4.4'; Calls = 0 }
        @{ Found = [version]'3.25.0'; Calls = 0 }
        @{ Found = [version]'3.24.3'; Calls = 1 }
        @{ Found = $null; Calls = 1 }
    )) {
    Set-Stub { param($Line) @{ ExitCode = 0; Output = @() } }
    $null = Install-MoTrackTool -Name CMake -Id Kitware.CMake -Version 4.4.4 -Found $case.Found -Minimum 3.25 6>&1
    Assert-True ($calls.Count -eq $case.Calls) "CMake $($case.Found): calls [$($calls -join '; ')]"
    if ($case.Calls) { Assert-True ($calls[0] -eq "install --id Kitware.CMake --version 4.4.4 $switches") "CMake $($case.Found): [$($calls[0])]" }
}

# -- LLVM: one install, of the pinned major ---------------------------------------------------------
function New-Registration {
    param([string]$Version, [string]$ProductCode, [string]$UninstallString, [string]$QuietUninstallString, [string]$Location)
    [pscustomobject]@{
        Version              = $Version
        Major                = if ($Version -match '^(\d+)') { [int]$Matches[1] } else { $null }
        ProductCode          = if ($ProductCode) { $ProductCode } else { $null }
        UninstallString      = if ($UninstallString) { $UninstallString } elseif ($ProductCode) { "MsiExec.exe /X$ProductCode" } else { '"C:\Program Files\LLVM\Uninstall.exe"' }
        QuietUninstallString = $QuietUninstallString
        Location             = if ($Location) { $Location } else { 'C:\Program Files\LLVM' }
        Key                  = "HKCU:\Software\MoTrackTests\absent-$([guid]::NewGuid().ToString('N'))"
    }
}
$LlvmDir = 'C:\Program Files\LLVM'
$msi22 = New-Registration -Version '22.1.5' -ProductCode '{22222222-2222-2222-2222-222222222222}'
$nsis18 = New-Registration -Version '18.1.8'
$msi23a = New-Registration -Version '23.1.5' -ProductCode '{F51D2584-CF0B-491B-8D94-5883E3B254B0}'
$msi23b = New-Registration -Version '23.1.6' -ProductCode '{2B2859B7-37CF-4E78-B617-72952CADA59B}'
$nsisElsewhere = New-Registration -Version '20.1.8' -UninstallString '"D:\LLVM\Uninstall.exe"' -Location 'D:\LLVM'
$ReplaceLlvm = $true

foreach ($case in @(
        @{ Name = 'none installed'; Installed = @(); Clang = $null; Action = 'Install'; Remove = 0; Note = $null }
        @{ Name = 'unregistered LLVM 22'; Installed = @(); Clang = 22; Action = 'Install'; Remove = 0; Note = '*LLVM 22 is in*without an installer registration; installing LLVM 23 over it*' }
        @{ Name = 'unregistered LLVM 23'; Installed = @(); Clang = 23; Action = 'Keep'; Remove = 0; Note = '*LLVM 23 is in*without an installer registration.' }
        @{ Name = 'pinned major, other release'; Installed = @($msi23b); Clang = 23; Action = 'Keep'; Remove = 0; Note = $null }
        @{ Name = 'pinned major x2'; Installed = @($msi23a, $msi23b); Clang = 23; Action = 'Keep'; Remove = 0; Note = '2 installer registrations of LLVM 23 (23.1.5, 23.1.6)*' }
        @{ Name = 'NSIS 18'; Installed = @($nsis18); Clang = 18; Action = 'Replace'; Remove = 1; Note = 'LLVM 18.1.8 is registered and*clang.exe is LLVM 18, but MoTrack builds with LLVM 23*' }
        @{ Name = 'MSI 22 + MSI 23'; Installed = @($msi22, $msi23b); Clang = 23; Action = 'Replace'; Remove = 2; Note = 'LLVM 22.1.5 is registered, but*(22.1.5, 23.1.6)*' }
        @{ Name = 'clang.exe missing'; Installed = @($msi23a); Clang = $null; Action = 'Replace'; Remove = 1; Note = "there's no clang.exe in*" }
    )) {
    $plan = Get-LlvmInstallPlan -Installed $case.Installed -PinnedMajor 23 -ClangMajor $case.Clang
    Assert-True ($plan.Action -eq $case.Action) "$($case.Name): action $($plan.Action)"
    Assert-True (@($plan.Remove).Count -eq $case.Remove) "$($case.Name): removes $(@($plan.Remove).Count)"
    if ($null -eq $case.Note) { Assert-True ($null -eq $plan.Note) "$($case.Name): note [$($plan.Note)]" }
    else { Assert-True ($plan.Note -like $case.Note) "$($case.Name): note [$($plan.Note)]" }
}

foreach ($case in @(
        @{ Registration = $msi22; FilePath = 'msiexec.exe'; Arguments = '/x {22222222-2222-2222-2222-222222222222} /passive /norestart' }
        @{ Registration = $nsis18; FilePath = 'C:\Program Files\LLVM\Uninstall.exe'; Arguments = '/S' }
        @{ Registration = (New-Registration -Version '18.1.8' -QuietUninstallString '"C:\Program Files\LLVM\Uninstall.exe" /S /quiet'); FilePath = 'C:\Program Files\LLVM\Uninstall.exe'; Arguments = '/S /quiet' }
    )) {
    $command = Get-LlvmUninstallCommand -Registration $case.Registration
    Assert-True ($command.FilePath -eq $case.FilePath -and $command.ArgumentList -eq $case.Arguments) "uninstall $($case.Registration.Version): [$($command.FilePath)] [$($command.ArgumentList)]"
}

# Install-PinnedLlvm against stubs. A successful msiexec /i moves the stub clang to $LlvmState.After.
$realGetInstalledLlvm = ${function:Get-InstalledLlvm}
$LlvmState = @{ Installed = @(); Release = $null; After = $LlvmVersion; ExitCode = 0; Hash = $LlvmSha256 }
function Get-InstalledLlvm { $LlvmState.Installed }
function Get-LlvmClangRelease { $LlvmState.Release }
function Invoke-WebRequest { param([string]$Uri, [string]$OutFile) $script:Calls.Add("download $Uri"); Set-Content -LiteralPath $OutFile -Value 'stub' -Encoding utf8 }
function Get-FileHash { param([string]$LiteralPath, [string]$Algorithm) [pscustomobject]@{ Hash = $LlvmState.Hash.ToUpperInvariant() } }
function Start-Process {
    param([string]$FilePath, [string]$ArgumentList, [switch]$Wait, [switch]$PassThru)
    $script:Calls.Add("start $FilePath $ArgumentList".Trim())
    if ($FilePath -eq 'msiexec.exe' -and $ArgumentList -like '/i *' -and $LlvmState.ExitCode -in 0, 3010) { $LlvmState.Release = $LlvmState.After }
    [pscustomobject]@{ ExitCode = $LlvmState.ExitCode }
}
function Set-LlvmStub {
    param([object[]]$Installed, [string]$Release, [string]$After = $LlvmVersion, [int]$ExitCode = 0, [string]$Hash = $LlvmSha256)
    $script:Calls.Clear()
    $LlvmState.Installed = @($Installed); $LlvmState.Release = $Release; $LlvmState.After = $After
    $LlvmState.ExitCode = $ExitCode; $LlvmState.Hash = $Hash
}
$download = "download https://github.com/llvm/llvm-project/releases/download/llvmorg-$LlvmVersion/LLVM-$LlvmVersion-win64.msi"
$msiexec = { param($Registration) "start msiexec.exe /x $($Registration.ProductCode) /passive /norestart" }

foreach ($case in @(
        @{ Name = 'none installed'; Installed = @(); Release = $null; Calls = @($download, 'start msiexec.exe /i *') }
        @{ Name = 'pinned major, other release'; Installed = @($msi23b); Release = '23.1.6'; Calls = @() }
        @{ Name = 'NSIS 18'; Installed = @($nsis18); Release = '18.1.8'; Calls = @($download, 'start C:\Program Files\LLVM\Uninstall.exe /S', 'start msiexec.exe /i *') }
        @{ Name = 'MSI 22 + MSI 23'; Installed = @($msi22, $msi23b); Release = '23.1.6'; Calls = @($download, (& $msiexec $msi22), (& $msiexec $msi23b), 'start msiexec.exe /i *') }
        @{ Name = 'NSIS elsewhere, LLVM 23 here'; Installed = @($nsisElsewhere, $msi23a); Release = '23.1.5'; Calls = @() }
        @{ Name = 'NSIS elsewhere only'; Installed = @($nsisElsewhere); Release = $null; Calls = @($download, 'start msiexec.exe /i *') }
    )) {
    Set-LlvmStub -Installed $case.Installed -Release $case.Release
    $text = (Install-PinnedLlvm 6>&1 | ForEach-Object { "$_" }) -join "`n"
    Assert-True ($calls.Count -eq $case.Calls.Count) "$($case.Name): calls [$($calls -join '; ')]"
    for ($i = 0; $i -lt $calls.Count; $i++) { Assert-True ($calls[$i] -like $case.Calls[$i]) "$($case.Name): call $i [$($calls[$i])]" }
    if ($case.Calls.Count -eq 0) {
        $suffix = if ($case.Release -ne $LlvmVersion) { " CI installs $LlvmVersion*" } else { '' }
        Assert-True ($text -like "*LLVM 23 is already installed; leaving it.$suffix") "$($case.Name): [$text]"
    }
    else { Assert-True ($text -like "*clang.exe is LLVM $LlvmVersion.*") "$($case.Name): [$text]" }
}

foreach ($case in @(
        @{ Name = 'SHA-256 mismatch'; Installed = @(); Release = $null; Hash = ('0' * 64); Message = "LLVM-$LlvmVersion-win64.msi has SHA-256 0000*Nothing was uninstalled or installed."; Calls = 1 }
        @{ Name = 'SHA-256 mismatch before a replace'; Installed = @($nsis18); Release = '18.1.8'; Hash = ('0' * 64); Message = '*has SHA-256 0000*Nothing was uninstalled or installed.'; Calls = 1 }
        @{ Name = 'msiexec fails'; Installed = @(); Release = $null; ExitCode = 1603; Message = "Installing LLVM-$LlvmVersion-win64.msi failed with msiexec exit code 1603*"; Calls = 2 }
        @{ Name = 'wrong release afterwards'; Installed = @(); Release = $null; After = '22.1.8'; Message = "After installing LLVM $LlvmVersion, *reports LLVM 22.1.8*"; Calls = 2 }
        @{ Name = 'uninstall fails'; Installed = @($nsis18); Release = '18.1.8'; ExitCode = 2; Message = 'Uninstalling LLVM 18.1.8 failed with exit code 2*'; Calls = 2 }
    )) {
    $stub = @{ Installed = $case.Installed; Release = $case.Release }
    foreach ($key in 'After', 'ExitCode', 'Hash') { if ($case.ContainsKey($key)) { $stub[$key] = $case[$key] } }
    Set-LlvmStub @stub
    $thrown = $null
    try { $null = Install-PinnedLlvm 6>$null 3>$null } catch { $thrown = $_.Exception.Message }
    Assert-True ($thrown -like $case.Message) "$($case.Name): [$thrown]"
    Assert-True ($calls.Count -eq $case.Calls) "$($case.Name): calls [$($calls -join '; ')]"
}

# Without -ReplaceLlvm, nothing is downloaded or uninstalled: a real run stops, a dry run says it would.
$ReplaceLlvm = $false
Set-LlvmStub -Installed @($nsis18, $nsisElsewhere) -Release '18.1.8'
$thrown = $null
try { $null = Install-PinnedLlvm 6>$null } catch { $thrown = $_.Exception.Message }
Assert-True ($thrown -like '*first means uninstalling LLVM 18.1.8 there. Rerun with -ReplaceLlvm*') "no -ReplaceLlvm: [$thrown]"
Assert-True ($calls.Count -eq 0) "no -ReplaceLlvm: calls [$($calls -join '; ')]"
$DryRun = $true
$text = @(Install-PinnedLlvm 6>&1 | ForEach-Object { "$_" })
Assert-True ($calls.Count -eq 0) "no -ReplaceLlvm dry-run: calls [$($calls -join '; ')]"
$dryRunLines = @($text | Where-Object { $_ -like '`[dry-run`]*' })
Assert-True ($dryRunLines.Count -eq 1 -and $dryRunLines[0] -like '`[dry-run`] stop: *Rerun with -ReplaceLlvm*') "no -ReplaceLlvm dry-run: [$($text -join "`n")]"
Assert-True (@($text | Where-Object { $_ -like '*Registered: LLVM 18.1.8 (NSIS .exe) in C:\Program Files\LLVM' }).Count -eq 1) "registered line lacks the location: [$($text -join "`n")]"
Assert-True (@($text | Where-Object { $_ -like '*Registered: LLVM 20.1.8 (NSIS .exe) in D:\LLVM; leaving it*' }).Count -eq 1) "registration elsewhere is not left: [$($text -join "`n")]"

# -DryRun -ReplaceLlvm prints the download, each uninstall, the install and the check, in that order.
$ReplaceLlvm = $true
Set-LlvmStub -Installed @($msi22, $msi23b) -Release '23.1.6'
$lines = @(Install-PinnedLlvm 6>&1 | ForEach-Object { "$_" } | Where-Object { $_ -like '`[dry-run`]*' })
$DryRun = $false
Assert-True ($calls.Count -eq 0) "LLVM dry-run: calls [$($calls -join '; ')]"
$expected = @(
    "[dry-run] $download (SHA-256 $LlvmSha256)"
    "[dry-run] msiexec.exe /x $($msi22.ProductCode) /passive /norestart"
    "[dry-run] msiexec.exe /x $($msi23b.ProductCode) /passive /norestart"
    "[dry-run] msiexec.exe /i *LLVM-$LlvmVersion-win64.msi`" /passive /norestart /l*v *"
    "[dry-run] C:\Program Files\LLVM\bin\clang.exe --version (must report LLVM $LlvmVersion)"
)
Assert-True ($lines.Count -eq $expected.Count) "LLVM dry-run: lines [$($lines -join "`n")]"
for ($i = 0; $i -lt $lines.Count; $i++) { Assert-True ($lines[$i] -like (ConvertTo-Pattern $expected[$i])) "LLVM dry-run line $i [$($lines[$i])]" }

# The registry reader, against a scratch uninstall root. LLVM's NSIS installers (before 23) record no
# InstallLocation, only $INSTDIR\Uninstall.exe, so the directory comes from the uninstaller's path.
$scratch = "HKCU:\Software\MoTrackTests-setup-dev-$([guid]::NewGuid().ToString('N'))"
try {
    $entries = @(
        @{ Key = 'LLVM'; Values = @{ DisplayName = 'LLVM'; DisplayVersion = '18.1.8'; UninstallString = '"C:\Program Files\LLVM\Uninstall.exe"' } }
        @{ Key = '{C261B515-7573-457D-AA83-FE1826A77ECE}'; Values = @{ DisplayName = 'LLVM'; DisplayVersion = '23.1.5'; WindowsInstaller = 1; InstallLocation = 'C:\Program Files\LLVM\'; UninstallString = 'MsiExec.exe /X{C261B515-7573-457D-AA83-FE1826A77ECE}' } }
        @{ Key = 'LLVM32'; Values = @{ DisplayName = 'LLVM'; DisplayVersion = '21.1.0'; UninstallString = '"C:\Program Files (x86)\LLVM\Uninstall.exe"' } }
        @{ Key = 'LLVMD'; Values = @{ DisplayName = 'LLVM'; DisplayVersion = '20.1.8'; UninstallString = 'D:\LLVM\Uninstall.exe' } }
        @{ Key = '{0D0D0D0D-0000-4000-8000-000000000000}'; Values = @{ DisplayName = 'LLVM'; DisplayVersion = '22.1.0'; WindowsInstaller = 1; UninstallString = 'MsiExec.exe /X{0D0D0D0D-0000-4000-8000-000000000000}' } }
        @{ Key = 'OpenMP'; Values = @{ DisplayName = 'LLVM OpenMP Runtime'; DisplayVersion = '19.0.0' } }
    )
    foreach ($entry in $entries) {
        $path = Join-Path $scratch $entry.Key
        $null = New-Item -Path $path -Force
        foreach ($name in $entry.Values.Keys) {
            $type = if ($entry.Values[$name] -is [int]) { 'DWord' } else { 'String' }
            $null = New-ItemProperty -LiteralPath $path -Name $name -Value $entry.Values[$name] -PropertyType $type
        }
    }
    $LlvmUninstallKeys = @($scratch)
    $found = @{}
    foreach ($registration in & $realGetInstalledLlvm) { $found[$registration.Version] = $registration }
    Assert-True ($found.Count -eq 5 -and -not $found.ContainsKey('19.0.0')) "registry: found [$($found.Keys -join ', ')]"
    Assert-True ($found['18.1.8'].Location -eq 'C:\Program Files\LLVM' -and $null -eq $found['18.1.8'].ProductCode) "registry: NSIS here [$($found['18.1.8'] | Out-String)]"
    Assert-True ($found['23.1.5'].Location -eq 'C:\Program Files\LLVM' -and $found['23.1.5'].ProductCode -eq '{C261B515-7573-457D-AA83-FE1826A77ECE}') "registry: MSI here [$($found['23.1.5'] | Out-String)]"
    Assert-True ($found['21.1.0'].Location -eq 'C:\Program Files (x86)\LLVM') "registry: 32-bit NSIS [$($found['21.1.0'].Location)]"
    Assert-True ($found['20.1.8'].Location -eq 'D:\LLVM') "registry: NSIS elsewhere [$($found['20.1.8'].Location)]"
    Assert-True ($null -eq $found['22.1.0'].Location) "registry: MSI without InstallLocation [$($found['22.1.0'].Location)]"

    # Install-PinnedLlvm against that registry: only the NSIS 18 and MSI 23 in C:\Program Files\LLVM count.
    ${function:Get-InstalledLlvm} = $realGetInstalledLlvm
    $DryRun = $true
    $ReplaceLlvm = $false
    $LlvmState.Release = '23.1.5'
    $text = @(Install-PinnedLlvm 6>&1 | ForEach-Object { "$_" })
    $DryRun = $false
    Assert-True (@($text | Where-Object { $_ -like '`[dry-run`] stop: *uninstalling LLVM 18.1.8, LLVM 23.1.5 there*' }).Count -eq 1) "registry plan: [$($text -join "`n")]"
    foreach ($elsewhere in 'C:\Program Files (x86)\LLVM', 'D:\LLVM', 'an unrecorded directory') {
        Assert-True (@($text | Where-Object { $_ -like "*Registered: LLVM * in $elsewhere; leaving it*" }).Count -eq 1) "registry plan leaves $elsewhere`: [$($text -join "`n")]"
    }
} finally {
    Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue
}

# -- tools/dev-env.ps1 finds the one .dev-tools a checkout shares with its linked worktrees ------------
# A scratch repository (git without the user's configuration), a linked worktree of it and a plain
# directory, each checkout with a copy of the tools dev-env.ps1 runs. A fake SDK is Qt's and FFmpeg's
# CMake package files holding a label; the probe prints the labels of the SDKs dev-env.ps1 activated.
$pwsh = (Get-Process -Id $PID).Path
if (-not (Get-Command git -CommandType Application -ErrorAction SilentlyContinue)) {
    Write-Warning 'git is not on PATH; skipping the dev-env.ps1 worktree tests.'
} else {
    $work = Join-Path ([IO.Path]::GetTempPath()) "motrack-dev-env-tests-$([guid]::NewGuid().ToString('N'))"
    $savedEnv = @{}
    foreach ($name in 'GIT_CONFIG_GLOBAL', 'GIT_CONFIG_NOSYSTEM', 'MOTRACK_DEV_TOOLS', 'CMAKE_PREFIX_PATH', 'MOTRACK_FFMPEG_ROOT') {
        $savedEnv[$name] = [Environment]::GetEnvironmentVariable($name)
    }
    function Invoke-ScratchGit {
        git @args
        if ($LASTEXITCODE -ne 0) { throw "git $($args -join ' ') exited $LASTEXITCODE" }
    }
    function New-FakeSdk {
        param([string]$DevTools, [string]$Label)
        foreach ($file in "windows\qt\$($pinned.MOTRACK_QT_VERSION)\msvc2022_64\lib\cmake\Qt6\Qt6Config.cmake", 'vcpkg\installed\x64-windows\share\ffmpeg\FindFFMPEG.cmake') {
            $path = Join-Path $DevTools $file
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $path) | Out-Null
            Set-Content -LiteralPath $path -Value $Label -Encoding utf8
        }
    }
    function Get-ActivatedSdk {
        param([string]$Checkout)
        $lines = @(& $pwsh -NoProfile -File $probe -Checkout $Checkout 2>&1 | ForEach-Object { "$_" })
        Assert-True ($LASTEXITCODE -eq 0) "dev-env.ps1 in $Checkout exited $LASTEXITCODE`:`n$($lines -join "`n")"
        return $lines[-1]
    }
    try {
        New-Item -ItemType Directory -Path $work | Out-Null
        $env:GIT_CONFIG_GLOBAL = Join-Path $work 'gitconfig'
        $env:GIT_CONFIG_NOSYSTEM = '1'
        Set-Content -LiteralPath $env:GIT_CONFIG_GLOBAL -Value '' -Encoding utf8
        Remove-Item Env:MOTRACK_DEV_TOOLS, Env:CMAKE_PREFIX_PATH, Env:MOTRACK_FFMPEG_ROOT -ErrorAction SilentlyContinue
        $main = Join-Path $work 'main'
        $linked = Join-Path $work 'linked'
        $plain = Join-Path $work 'plain'
        Invoke-ScratchGit init --quiet $main
        Invoke-ScratchGit -C $main -c user.name=MoTrack -c user.email=motrack@example.invalid commit --quiet --allow-empty -m init
        Invoke-ScratchGit -C $main worktree add --quiet --detach $linked
        foreach ($checkout in $linked, $plain) {
            $tools = (New-Item -ItemType Directory -Force -Path (Join-Path $checkout 'tools')).FullName
            foreach ($file in 'dev-env.ps1', 'dev_sdk.py', 'media_sdk.py', 'toolchain.env') {
                Copy-Item -LiteralPath (Join-Path $PSScriptRoot $file) -Destination $tools
            }
        }
        $probe = Join-Path $work 'probe.ps1'
        Set-Content -LiteralPath $probe -Encoding utf8 -Value @'
#Requires -Version 7
# Activates a checkout's tools/dev-env.ps1, then prints the labels of the Qt and FFmpeg it found.
param([string]$Checkout)
# Only the SDK lookup is under test, so Visual Studio's developer shell is skipped.
$env:VCToolsInstallDir = 'C:\stub\'
$env:WindowsSdkDir = 'C:\stub\'
$env:VSCMD_ARG_TGT_ARCH = 'x64'
. (Join-Path $Checkout 'tools\dev-env.ps1')
function Get-Label {
    param([string]$Prefix, [string]$File)
    if ($Prefix -and (Test-Path -LiteralPath (Join-Path $Prefix $File))) { return Get-Content -LiteralPath (Join-Path $Prefix $File) -Encoding utf8 -TotalCount 1 }
    return 'none'
}
$qt = @("$env:CMAKE_PREFIX_PATH" -split ';')[0]
"$(Get-Label $qt 'lib\cmake\Qt6\Qt6Config.cmake') $(Get-Label "$env:MOTRACK_FFMPEG_ROOT" 'share\ffmpeg\FindFFMPEG.cmake')"
'@
        New-FakeSdk -DevTools (Join-Path $main '.dev-tools') -Label main
        $found = Get-ActivatedSdk $linked
        Assert-True ($found -eq 'main main') "worktree without a .dev-tools: activated [$found]"
        Assert-True (-not (Test-Path -LiteralPath (Join-Path $linked '.dev-tools'))) 'dev-env.ps1 created a .dev-tools in the worktree'
        New-Item -ItemType Directory -Path (Join-Path $linked '.dev-tools') | Out-Null
        $found = Get-ActivatedSdk $linked
        Assert-True ($found -eq 'main main') "worktree with an empty .dev-tools: activated [$found]"
        $env:MOTRACK_DEV_TOOLS = Join-Path $work 'override'
        New-FakeSdk -DevTools $env:MOTRACK_DEV_TOOLS -Label override
        $found = Get-ActivatedSdk $linked
        Assert-True ($found -eq 'override override') "MOTRACK_DEV_TOOLS: activated [$found]"
        Remove-Item Env:MOTRACK_DEV_TOOLS
        New-FakeSdk -DevTools (Join-Path $linked '.dev-tools') -Label linked
        $found = Get-ActivatedSdk $linked
        Assert-True ($found -eq 'linked linked') "worktree with its own SDKs: activated [$found]"
        # Not a git checkout: git has no main checkout to offer, so only its own .dev-tools (none) counts.
        $found = Get-ActivatedSdk $plain
        Assert-True ($found -eq 'none none') "plain directory: activated [$found]"
    } finally {
        foreach ($name in $savedEnv.Keys) { [Environment]::SetEnvironmentVariable($name, $savedEnv[$name]) }
        if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
    }
}

# -- The whole script under -DryRun, with a stub winget.cmd first on PATH ---------------------------
$root = Join-Path ([IO.Path]::GetTempPath()) "motrack-setup-dev-tests-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $root | Out-Null
$savedPath = $env:Path
$savedDevTools = $env:MOTRACK_DEV_TOOLS
try {
    [IO.File]::WriteAllText((Join-Path $root 'winget.cmd'), "@echo STUB WINGET RAN %*`r`n@exit /b 1`r`n")
    $env:Path = "$root;$env:Path"
    # Setup installs the SDKs into the .dev-tools that tools/dev_sdk.py dev-tools resolves.
    $env:MOTRACK_DEV_TOOLS = Join-Path $root 'shared-sdk'
    $output = & $pwsh -NoProfile -File $setup -DryRun -Build 2>&1 | Out-String
    Assert-True ($LASTEXITCODE -eq 0) "setup-dev.ps1 -DryRun exited $LASTEXITCODE`:`n$output"
    Assert-True (-not $output.Contains('STUB WINGET RAN')) "-DryRun ran winget:`n$output"
    if (Get-MoTrackPython) {
        foreach ($step in 'dev_sdk.py', 'media_sdk.py') {
            Assert-True ($output -match "\[dry-run\] python tools/$([regex]::Escape($step)) install \(.*, into $([regex]::Escape($env:MOTRACK_DEV_TOOLS))\)") "-DryRun does not install $step into MOTRACK_DEV_TOOLS:`n$output"
        }
    }
    Assert-True ($output.Contains("LLVM $LlvmVersion, CMake $($pinned.MOTRACK_CMAKE_VERSION), Ninja $($pinned.MOTRACK_NINJA_VERSION)")) "-DryRun does not print the pins:`n$output"
    # The LLVM step depends on this machine: it installs the pin, keeps the pinned major, or (on a CI
    # image with another LLVM registered in C:\Program Files\LLVM) stops for -ReplaceLlvm.
    Assert-True ($output.Contains("[dry-run] download https://github.com/llvm/llvm-project/releases/download/llvmorg-$LlvmVersion/") -or $output -match "LLVM $LlvmMajor is already installed; leaving it\." -or $output -match '\[dry-run\] stop: .*Rerun with -ReplaceLlvm') "-DryRun has no LLVM install, keep or stop line:`n$output"
    Assert-True ($output.Contains('[dry-run] cmake --workflow --preset dev')) "-DryRun -Build does not preview the build:`n$output"
    Assert-True ($output -notmatch 'VisualStudio\.20\d\d\.BuildTools') "-DryRun installs a year-specific Visual Studio package:`n$output"
    Write-Host 'setup-dev.ps1 tests passed'
} finally {
    $env:Path = $savedPath
    $env:MOTRACK_DEV_TOOLS = $savedDevTools
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
