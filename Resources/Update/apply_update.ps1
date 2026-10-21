# Copyright (c) 2026 CarlxsMG.
#
# Installs a downloaded UEAgent update once the editor has closed: while the editor runs it holds the
# plugin's DLLs, so their folder cannot be replaced. The plugin starts this script when the editor
# exits with an update ready (Source/UEAgent/Private/Mcp/McpUpdate.cpp), from a copy in
# Saved/UEAgentUpdate: never from the plugin folder it replaces.
#
# It waits for the editor to close, moves the plugin folder to BackupDir and the downloaded one in. If
# the new one cannot be moved in, the old one goes back. The result is one line in LogPath, which the
# plugin reads on the next startup. With EditorExe and Project, it opens the editor again afterwards.

param(
    [Parameter(Mandatory = $true)][int]$EditorPid,
    [Parameter(Mandatory = $true)][string]$PluginDir,
    [Parameter(Mandatory = $true)][string]$StagedPlugin,
    [Parameter(Mandatory = $true)][string]$BackupDir,
    [Parameter(Mandatory = $true)][string]$Version,
    [Parameter(Mandatory = $true)][string]$LogPath,
    [string]$EditorExe = '',
    [string]$Project = ''
)

$ErrorActionPreference = 'Stop'

function Write-Result([string]$Text) {
    Add-Content -LiteralPath $LogPath -Value ('{0} {1}' -f (Get-Date).ToUniversalTime().ToString('yyyy-MM-dd HH:mm:ssZ'), $Text)
}

try {
    # Closing can take a while (saving, shutting down subsystems): up to 15 minutes.
    Wait-Process -Id $EditorPid -Timeout 900 -ErrorAction SilentlyContinue
    if (Get-Process -Id $EditorPid -ErrorAction SilentlyContinue) {
        throw 'the editor did not close within 15 minutes'
    }
    # Let Windows release the plugin's files.
    Start-Sleep -Seconds 2

    if (-not (Test-Path -LiteralPath (Join-Path $StagedPlugin 'UEAgent.uplugin'))) {
        throw "there is no update in $StagedPlugin"
    }
    if (Test-Path -LiteralPath $BackupDir) {
        Remove-Item -LiteralPath $BackupDir -Recurse -Force
    }
    # A rename on the same volume: the project's Saved and Plugins folders are both in the project.
    Move-Item -LiteralPath $PluginDir -Destination $BackupDir
    try {
        Move-Item -LiteralPath $StagedPlugin -Destination $PluginDir
    }
    catch {
        $Reason = $_.Exception.Message
        if (Test-Path -LiteralPath $PluginDir) {
            Remove-Item -LiteralPath $PluginDir -Recurse -Force
        }
        Move-Item -LiteralPath $BackupDir -Destination $PluginDir
        throw "the new version could not be moved in, and the previous one was put back: $Reason"
    }
    Write-Result "OK $Version"
}
catch {
    Write-Result "FAILED $Version`: $($_.Exception.Message)"
}

if ($EditorExe -and $Project) {
    Start-Process -FilePath $EditorExe -ArgumentList ('"{0}"' -f $Project)
}
