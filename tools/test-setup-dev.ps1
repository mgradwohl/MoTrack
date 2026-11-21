# Tests for tools/setup-dev.ps1, adapted from TaskSmack's tools/test-setup-dev.ps1. The script is
# dot-sourced (it returns before its install steps then) and its functions run against stub winget,
# Start-Process, Invoke-WebRequest and installed-LLVM readers that record their calls; nothing is
# installed, uninstalled or downloaded. The registry reader runs against a scratch key under HKCU that
# the test deletes. Last, the whole script runs with -DryRun and a stub winget.cmd first on PATH.
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
    param([string]$Version, [string]$ProductCode, [string]$UninstallString, [string]$QuietUninstallString)
    [pscustomobject]@{
        Version              = $Version
        Major                = if ($Version -match '^(\d+)') { [int]$Matches[1] } else { $null }
        ProductCode          = if ($ProductCode) { $ProductCode } else { $null }
        UninstallString      = if ($UninstallString) { $UninstallString } elseif ($ProductCode) { "MsiExec.exe /X$ProductCode" } else { '"C:\Program Files\LLVM\Uninstall.exe"' }
        QuietUninstallString = $QuietUninstallString
        Key                  = "HKCU:\Software\MoTrackTests\absent-$([guid]::NewGuid().ToString('N'))"
    }
}
$LlvmDir = 'C:\Program Files\LLVM'
$msi22 = New-Registration -Version '22.1.5' -ProductCode '{22222222-2222-2222-2222-222222222222}'
$nsis18 = New-Registration -Version '18.1.8'
$msi23a = New-Registration -Version '23.1.5' -ProductCode '{F51D2584-CF0B-491B-8D94-5883E3B254B0}'
$msi23b = New-Registration -Version '23.1.6' -ProductCode '{2B2859B7-37CF-4E78-B617-72952CADA59B}'

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
function Invoke-WebRequest { param([string]$Uri, [string]$OutFile) $script:Calls.Add("download $Uri"); Set-Content -LiteralPath $OutFile -Value 'stub' }
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
        @{ Name = 'NSIS 18'; Installed = @($nsis18); Release = '18.1.8'; Calls = @('start C:\Program Files\LLVM\Uninstall.exe /S', $download, 'start msiexec.exe /i *') }
        @{ Name = 'MSI 22 + MSI 23'; Installed = @($msi22, $msi23b); Release = '23.1.6'; Calls = @((& $msiexec $msi22), (& $msiexec $msi23b), $download, 'start msiexec.exe /i *') }
    )) {
    Set-LlvmStub -Installed $case.Installed -Release $case.Release
    $text = (Install-PinnedLlvm 6>&1 | ForEach-Object { "$_" }) -join "`n"
    Assert-True ($calls.Count -eq $case.Calls.Count) "$($case.Name): calls [$($calls -join '; ')]"
    for ($i = 0; $i -lt $calls.Count; $i++) { Assert-True ($calls[$i] -like $case.Calls[$i]) "$($case.Name): call $i [$($calls[$i])]" }
    if ($case.Calls.Count -eq 0) { Assert-True ($text -like "*LLVM 23 is already installed; leaving it. CI installs $LlvmVersion*") "$($case.Name): [$text]" }
    else { Assert-True ($text -like "*clang.exe is LLVM $LlvmVersion.*") "$($case.Name): [$text]" }
}

foreach ($case in @(
        @{ Name = 'SHA-256 mismatch'; Installed = @(); Release = $null; Hash = ('0' * 64); Message = "LLVM-$LlvmVersion-win64.msi has SHA-256 0000*Nothing was installed."; Calls = 1 }
        @{ Name = 'msiexec fails'; Installed = @(); Release = $null; ExitCode = 1603; Message = "Installing LLVM-$LlvmVersion-win64.msi failed with msiexec exit code 1603*"; Calls = 2 }
        @{ Name = 'wrong release afterwards'; Installed = @(); Release = $null; After = '22.1.8'; Message = "After installing LLVM $LlvmVersion, *reports LLVM 22.1.8*"; Calls = 2 }
        @{ Name = 'uninstall fails'; Installed = @($nsis18); Release = '18.1.8'; ExitCode = 2; Message = 'Uninstalling LLVM 18.1.8 failed with exit code 2*'; Calls = 1 }
    )) {
    $stub = @{ Installed = $case.Installed; Release = $case.Release }
    foreach ($key in 'After', 'ExitCode', 'Hash') { if ($case.ContainsKey($key)) { $stub[$key] = $case[$key] } }
    Set-LlvmStub @stub
    $thrown = $null
    try { $null = Install-PinnedLlvm 6>$null 3>$null } catch { $thrown = $_.Exception.Message }
    Assert-True ($thrown -like $case.Message) "$($case.Name): [$thrown]"
    Assert-True ($calls.Count -eq $case.Calls) "$($case.Name): calls [$($calls -join '; ')]"
}

# -DryRun prints each uninstall, the download, the install and the check, and runs none of them.
$DryRun = $true
Set-LlvmStub -Installed @($msi22, $msi23b) -Release '23.1.6'
$lines = @(Install-PinnedLlvm 6>&1 | ForEach-Object { "$_" } | Where-Object { $_ -like '`[dry-run`]*' })
$DryRun = $false
Assert-True ($calls.Count -eq 0) "LLVM dry-run: calls [$($calls -join '; ')]"
$expected = @(
    "[dry-run] msiexec.exe /x $($msi22.ProductCode) /passive /norestart"
    "[dry-run] msiexec.exe /x $($msi23b.ProductCode) /passive /norestart"
    "[dry-run] $download (SHA-256 $LlvmSha256)"
    "[dry-run] msiexec.exe /i *LLVM-$LlvmVersion-win64.msi`" /passive /norestart /l*v *"
    "[dry-run] C:\Program Files\LLVM\bin\clang.exe --version (must report LLVM $LlvmVersion)"
)
Assert-True ($lines.Count -eq $expected.Count) "LLVM dry-run: lines [$($lines -join "`n")]"
for ($i = 0; $i -lt $lines.Count; $i++) { Assert-True ($lines[$i] -like (ConvertTo-Pattern $expected[$i])) "LLVM dry-run line $i [$($lines[$i])]" }

# The registry reader, against a scratch uninstall root: only the LLVM installs in $LlvmDir count.
$scratch = "HKCU:\Software\MoTrackTests-setup-dev-$([guid]::NewGuid().ToString('N'))"
try {
    $entries = @(
        @{ Key = 'LLVM'; Values = @{ DisplayName = 'LLVM'; DisplayVersion = '18.1.8'; UninstallString = '"C:\Program Files\LLVM\Uninstall.exe"' } }
        @{ Key = '{C261B515-7573-457D-AA83-FE1826A77ECE}'; Values = @{ DisplayName = 'LLVM'; DisplayVersion = '23.1.5'; WindowsInstaller = 1; InstallLocation = 'C:\Program Files\LLVM\'; UninstallString = 'MsiExec.exe /X{C261B515-7573-457D-AA83-FE1826A77ECE}' } }
        @{ Key = 'LLVM32'; Values = @{ DisplayName = 'LLVM'; DisplayVersion = '21.1.0'; InstallLocation = 'C:\Program Files (x86)\LLVM' } }
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
    $found = @(& $realGetInstalledLlvm | Sort-Object Version)
    Assert-True ($found.Count -eq 2) "registry: found [$(($found | ForEach-Object Version) -join ', ')]"
    Assert-True ($found[0].Version -eq '18.1.8' -and $null -eq $found[0].ProductCode) "registry: NSIS entry [$($found[0] | Out-String)]"
    Assert-True ($found[1].Major -eq 23 -and $found[1].ProductCode -eq '{C261B515-7573-457D-AA83-FE1826A77ECE}') "registry: MSI entry [$($found[1] | Out-String)]"
} finally {
    Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue
}

# -- The whole script under -DryRun, with a stub winget.cmd first on PATH ---------------------------
$root = Join-Path ([IO.Path]::GetTempPath()) "motrack-setup-dev-tests-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $root | Out-Null
$savedPath = $env:Path
try {
    [IO.File]::WriteAllText((Join-Path $root 'winget.cmd'), "@echo STUB WINGET RAN %*`r`n@exit /b 1`r`n")
    $env:Path = "$root;$env:Path"
    $pwsh = (Get-Process -Id $PID).Path
    $output = & $pwsh -NoProfile -File $setup -DryRun -Build 2>&1 | Out-String
    Assert-True ($LASTEXITCODE -eq 0) "setup-dev.ps1 -DryRun exited $LASTEXITCODE`:`n$output"
    Assert-True (-not $output.Contains('STUB WINGET RAN')) "-DryRun ran winget:`n$output"
    Assert-True ($output.Contains("LLVM $LlvmVersion, CMake $($pinned.MOTRACK_CMAKE_VERSION), Ninja $($pinned.MOTRACK_NINJA_VERSION)")) "-DryRun does not print the pins:`n$output"
    Assert-True ($output.Contains("[dry-run] download https://github.com/llvm/llvm-project/releases/download/llvmorg-$LlvmVersion/") -or $output -match "LLVM $LlvmMajor is already installed; leaving it\.") "-DryRun has no LLVM install or keep line:`n$output"
    Assert-True ($output.Contains('[dry-run] cmake --workflow --preset dev')) "-DryRun -Build does not preview the build:`n$output"
    Assert-True ($output -notmatch 'VisualStudio\.20\d\d\.BuildTools') "-DryRun installs a year-specific Visual Studio package:`n$output"
    Write-Host 'setup-dev.ps1 tests passed'
} finally {
    $env:Path = $savedPath
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
