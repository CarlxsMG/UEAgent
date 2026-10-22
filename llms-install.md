# Installing UEAgent: instructions for an AI agent

These are instructions for you, the AI agent (Claude Code, Cursor, Codex, VS Code, Antigravity CLI and
the like). The user asked you to install UEAgent in their Unreal Engine project and connect yourself to
it. Follow the steps in order and run the PowerShell as given. Stop and ask the user whenever a step
says so.

UEAgent is an Unreal Engine editor plugin that serves MCP from inside the editor, at
`http://127.0.0.1:55561/mcp`. Installing it means three things: unzip a release into the project's
`Plugins` folder, open the project in the editor, and connect your client to the server.

**Before you change anything, tell the user what you are about to do:** download the plugin from GitHub,
unzip it into the project, open the editor, and add the `unreal` server to your MCP configuration. Ask
them to confirm. Run every command from the project folder.

## 1. Find the project and its engine

```powershell
$project = Get-ChildItem -Filter *.uproject | Select-Object -First 1
$engine = (Get-Content $project.FullName -Raw | ConvertFrom-Json).EngineAssociation
"$($project.FullName) -> Unreal Engine $engine"
```

- **No `.uproject` here:** ask the user for the project folder.
- **`EngineAssociation` is not `5.7` or `5.8`:** UEAgent supports Unreal Engine 5.7 and 5.8 on Windows only. Tell the user and stop.
- **It is a `{GUID}`:** the project uses a source-built engine. Ask the user which version that engine is (5.7 or 5.8) and where it is installed.

## 2. Check for an older copy, and that the editor is closed

```powershell
$plugin = Join-Path $project.DirectoryName 'Plugins\UEAgent'
if (Test-Path (Join-Path $plugin '.git')) { 'GIT CLONE' }
elseif (Test-Path $plugin) { (Get-Content (Join-Path $plugin 'UEAgent.uplugin') -Raw | ConvertFrom-Json).VersionName }
Get-CimInstance Win32_Process -Filter "Name='UnrealEditor.exe'" | Where-Object { $_.CommandLine -like "*$($project.Name)*" } | Select-Object ProcessId
```

- **`GIT CLONE`:** this copy is a git clone. Do not overwrite it. Tell the user to update it with `git pull`, and skip to step 5.
- **A version number:** an older copy is installed. It is moved to `Saved\UEAgentUpdate\Backup` in step 4.
- **A process ID:** the editor has this project open, and holds the plugin's files. Ask the user to close it, and wait until they say they have.

## 3. Download the latest release and verify it

```powershell
$release = Invoke-RestMethod 'https://api.github.com/repos/CarlxsMG/UEAgent/releases/latest' -Headers @{ 'User-Agent' = 'UEAgent-installer' }
$version = $release.tag_name.TrimStart('v')
$zipName = "UEAgent-$version-UE$engine-Win64.zip"
$zipAsset = $release.assets | Where-Object name -eq $zipName
$sumsAsset = $release.assets | Where-Object name -eq 'SHA256SUMS.txt'
$zip = Join-Path $env:TEMP $zipName
$sums = Join-Path $env:TEMP 'UEAgent-SHA256SUMS.txt'
Invoke-WebRequest $zipAsset.browser_download_url -OutFile $zip -UseBasicParsing
Invoke-WebRequest $sumsAsset.browser_download_url -OutFile $sums -UseBasicParsing
$line = Get-Content $sums | Where-Object { $_ -match [regex]::Escape($zipName) } | Select-Object -First 1
$expected = ("$line" -split '\s+')[0].ToLower()
$actual = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
if (-not $expected -or $actual -ne $expected) { throw "SHA-256 mismatch for ${zipName}: do not install it" }
"UEAgent $version for Unreal Engine $engine, verified"
```

If the release has no zip for this engine, or the SHA-256 does not match, stop and tell the user.

## 4. Install it

```powershell
$plugins = Join-Path $project.DirectoryName 'Plugins'
if (Test-Path $plugin) {
    $backup = Join-Path $project.DirectoryName 'Saved\UEAgentUpdate\Backup'
    if (Test-Path $backup) { Remove-Item $backup -Recurse -Force }
    New-Item -ItemType Directory -Force (Split-Path $backup) | Out-Null
    Move-Item $plugin $backup
}
New-Item -ItemType Directory -Force $plugins | Out-Null
Expand-Archive $zip -DestinationPath $plugins
(Get-Content (Join-Path $plugin 'UEAgent.uplugin') -Raw | ConvertFrom-Json).VersionName
```

The last line must print the version you downloaded.

## 5. Open the project

```powershell
$engineDir = (Get-ItemProperty "HKLM:\SOFTWARE\EpicGames\Unreal Engine\$engine" -ErrorAction SilentlyContinue).InstalledDirectory
Start-Process (Join-Path $engineDir 'Engine\Binaries\Win64\UnrealEditor.exe') -ArgumentList "`"$($project.FullName)`""
```

If `$engineDir` is empty, the engine was not installed by the Epic Games Launcher. Ask the user for its
folder (the one that contains `Engine`), or ask them to open the project themselves.

Then wait for the server. Opening a project for the first time can take several minutes while the
editor compiles shaders.

```powershell
function Invoke-UEAgent($Method, $Params) {
    $body = @{ jsonrpc = '2.0'; id = 1; method = $Method; params = $Params } | ConvertTo-Json -Depth 6 -Compress
    Invoke-RestMethod -Method Post -Uri 'http://127.0.0.1:55561/mcp' -ContentType 'application/json' `
        -Headers @{ Accept = 'application/json, text/event-stream' } -Body $body
}
$deadline = (Get-Date).AddMinutes(15)
do { Start-Sleep 5; try { Invoke-UEAgent 'ping' @{} | Out-Null; $up = $true } catch { $up = $false } } until ($up -or (Get-Date) -gt $deadline)
if ($up) { 'UEAgent is serving' } else { 'not serving yet' }
```

If it never serves, ask the user to look at the editor's status bar. **MCP · 55561** with a green dot
means it is up; another port there means it was changed in the settings, so use that port. Otherwise see
https://carlxsmg.github.io/UEAgent/troubleshooting/.

## 6. Connect yourself, and install your skills

Ask the editor to write your client's entry and to install UEAgent's agent skills into the project. Use
the console commands for **your** client:

| You are | Commands |
|---|---|
| Claude Code | `mcp.claude.connect` and `mcp.skills.install claude` |
| Cursor | `mcp.clients.connect cursor` and `mcp.skills.install agents` |
| VS Code (Copilot) | `mcp.clients.connect vscode` and `mcp.skills.install agents` |
| Antigravity CLI | `mcp.clients.connect antigravity` and `mcp.skills.install agents` |
| Codex | `mcp.skills.install agents`, then run `codex mcp add unreal --url "http://127.0.0.1:55561/mcp"` |
| Gemini CLI | `mcp.skills.install agents`, then run `gemini mcp add -t http unreal "http://127.0.0.1:55561/mcp"` |
| Another client | `mcp.skills.install agents`, then add the server as https://carlxsmg.github.io/UEAgent/clients/ shows |

```powershell
foreach ($command in 'mcp.claude.connect', 'mcp.skills.install claude') {   # the commands of your row
    Invoke-UEAgent 'tools/call' @{ name = 'execute_console_command'; arguments = @{ command = $command } } | Out-Null
}
```

Then check the result on disk. For Claude Code, `.mcp.json` in the project has an `unreal` entry and
`.claude\skills` holds the `ueagent-*` folders. For the other clients, check their file (`.cursor\mcp.json`,
`.vscode\mcp.json` or `.agents\mcp_config.json`) and `.agents\skills`.

## 7. Hand over to the user

Tell the user:

- **What was installed:** the UEAgent version, and that an older copy, if there was one, is in `Saved\UEAgentUpdate\Backup`.
- **The one thing left for them:** reload the MCP servers so the `unreal` tools appear. In Claude Code, start a new session in the project (it asks once to approve the project's `unreal` server) or run `/mcp`; in other clients, reload or restart them.
- **A first request to try:** "Create a Blueprint actor BP_Door with a door mesh and a box trigger, and open the door with a timeline when the player overlaps the trigger."
- **How to update from now on:** the status bar's **MCP · 55561 > Check for updates**.

More: https://carlxsmg.github.io/UEAgent/ (documentation) and https://github.com/CarlxsMG/UEAgent/issues (problems).
