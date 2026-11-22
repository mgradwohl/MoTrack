#Requires -Version 7
# Scratch git repositories for tools/test-setup-dev.ps1 and tools/test-check-prereqs.ps1, which dot-source
# this file after Set-StrictMode. Every scratch git command runs without the caller's GIT_* variables: a git
# hook exports GIT_DIR, GIT_WORK_TREE and GIT_INDEX_FILE, which would aim the commands at the real
# repository. It also runs without the user's git configuration, so no hook, template or signing setting
# applies.

# The empty global configuration scratch git uses; Initialize-ScratchGit creates it.
$ScratchGitConfig = $null

# Creates the empty git configuration under -Root that Invoke-ScratchGit uses.
function Initialize-ScratchGit {
    param([Parameter(Mandatory)][string]$Root)
    $script:ScratchGitConfig = Join-Path $Root 'gitconfig'
    Set-Content -LiteralPath $script:ScratchGitConfig -Value '' -Encoding utf8
}

# Runs git with $args and only the scratch configuration; returns its output, and throws when it fails.
# The caller's GIT_* variables are set again afterwards.
function Invoke-ScratchGit {
    if (-not $ScratchGitConfig) { throw 'Initialize-ScratchGit first.' }
    $saved = @(Get-ChildItem Env: | Where-Object Name -like 'GIT_*' | ForEach-Object { [pscustomobject]@{ Name = $_.Name; Value = $_.Value } })
    try {
        foreach ($variable in $saved) { Remove-Item -LiteralPath "Env:$($variable.Name)" }
        $env:GIT_CONFIG_NOSYSTEM = '1'
        $env:GIT_CONFIG_GLOBAL = $ScratchGitConfig
        $output = git @args
        if ($LASTEXITCODE -ne 0) { throw "git $($args -join ' ') exited $LASTEXITCODE" }
        $output
    } finally {
        Remove-Item Env:GIT_CONFIG_NOSYSTEM, Env:GIT_CONFIG_GLOBAL -ErrorAction SilentlyContinue
        foreach ($variable in $saved) { Set-Item -LiteralPath "Env:$($variable.Name)" -Value $variable.Value }
    }
}

# Copies of tools/<Tools> in -Checkout, which make it a MoTrack checkout.
function Copy-ScratchTool {
    param([Parameter(Mandatory)][string]$Checkout, [Parameter(Mandatory)][string[]]$Tools)
    $directory = (New-Item -ItemType Directory -Force -Path (Join-Path $Checkout 'tools')).FullName
    foreach ($file in $Tools) { Copy-Item -LiteralPath (Join-Path $PSScriptRoot $file) -Destination $directory }
}

# A scratch MoTrack checkout at -Root\main, with one commit and copies of -Tools, and a linked worktree of
# it at -Root\linked with the same copies. Returns @{ Main; Linked }.
function New-ScratchWorktree {
    param([Parameter(Mandatory)][string]$Root, [Parameter(Mandatory)][string[]]$Tools)
    $main = Join-Path $Root 'main'
    $linked = Join-Path $Root 'linked'
    $null = Invoke-ScratchGit init --quiet $main
    $null = Invoke-ScratchGit -C $main -c user.name=MoTrack -c user.email=motrack@example.invalid commit --quiet --allow-empty -m init
    $null = Invoke-ScratchGit -C $main worktree add --quiet --detach $linked
    foreach ($checkout in $main, $linked) { Copy-ScratchTool -Checkout $checkout -Tools $Tools }
    [pscustomobject]@{ Main = $main; Linked = $linked }
}

# Makes a decoy repository at -Decoy and points GIT_DIR, GIT_WORK_TREE and GIT_INDEX_FILE at it, as a git
# hook does for its own repository. Returns its state, which Get-ScratchRepositoryState must still report
# after the scratch commands have run. The caller restores the variables.
function Set-HookGitEnvironment {
    param([Parameter(Mandatory)][string]$Decoy)
    $null = Invoke-ScratchGit init --quiet $Decoy
    $null = Invoke-ScratchGit -C $Decoy -c user.name=Decoy -c user.email=decoy@example.invalid commit --quiet --allow-empty -m decoy
    $env:GIT_DIR = Join-Path $Decoy '.git'
    $env:GIT_WORK_TREE = $Decoy
    $env:GIT_INDEX_FILE = Join-Path $Decoy '.git\index'
    Get-ScratchRepositoryState $Decoy
}

# A repository's commits and worktrees, as one line, to show that nothing committed to it or added a worktree.
function Get-ScratchRepositoryState {
    param([Parameter(Mandatory)][string]$Repository)
    $commits = Invoke-ScratchGit -C $Repository rev-list --all --count
    $worktrees = @(Invoke-ScratchGit -C $Repository worktree list --porcelain | Where-Object { $_ -like 'worktree *' }).Count
    "$commits commits, $worktrees worktrees"
}

# The environment variables -Names and every GIT_* variable, to give back to Restore-ScratchEnvironment.
function Save-ScratchEnvironment {
    param([string[]]$Names = @())
    $saved = @{}
    foreach ($name in $Names) { $saved[$name] = [Environment]::GetEnvironmentVariable($name) }
    foreach ($variable in Get-ChildItem Env: | Where-Object Name -like 'GIT_*') { $saved[$variable.Name] = $variable.Value }
    $saved
}

# Sets the variables Save-ScratchEnvironment returned back, and removes any GIT_* variable it did not.
function Restore-ScratchEnvironment {
    param([Parameter(Mandatory)][hashtable]$Saved)
    foreach ($variable in @(Get-ChildItem Env: | Where-Object Name -like 'GIT_*')) {
        if (-not $Saved.ContainsKey($variable.Name)) { Remove-Item -LiteralPath "Env:$($variable.Name)" }
    }
    foreach ($name in $Saved.Keys) { [Environment]::SetEnvironmentVariable($name, $Saved[$name]) }
}
