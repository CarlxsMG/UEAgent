# Getting started

From a project without UEAgent to an agent that builds in its editor, in four steps. Your agent can also
do them for you: see [Install with your agent](agent-install.md).

## What you need

- **Unreal Engine 5.7 or 5.8 on Windows** (Win64), the only platform the plugin declares. A few tools
  need 5.8: see the [FAQ](faq.md#which-unreal-engine-versions-does-it-support).
- **An MCP client** that speaks Streamable HTTP: Claude Code, Cursor, VS Code, Windsurf, Claude Desktop,
  Gemini CLI, Codex or another one.
- Nothing else. The release is precompiled, so no compiler is needed, in a Blueprint-only project or a C++
  one, and the server is C++ inside the editor, with no Python or Node.js process to run. Only Claude
  Desktop needs Node.js, for the bridge it connects through.

## 1. Install the plugin

Download `UEAgent-<version>-UE5.8-Win64.zip` (or `-UE5.7-`, for Unreal Engine 5.7) from the
[latest release](https://github.com/CarlxsMG/UEAgent/releases/latest) and unzip it into
`<YourProject>/Plugins/`.

To build it from source instead, copy or clone the [repository](https://github.com/CarlxsMG/UEAgent) to
`<YourProject>/Plugins/UEAgent`, then open the project and let Unreal build it, or build the editor target
from your IDE. That needs Visual Studio 2022 with the C++ toolchain.

## 2. Open the project

The server starts with the editor. Its status bar shows **MCP · 55561** with a green dot once the server
is up, and the Output Log says `MCP HTTP server started on http://127.0.0.1:55561/mcp`.

No green dot? Another program may hold the port: see
[A client cannot connect](troubleshooting.md#a-client-cannot-connect).

## 3. Connect your client

**Claude Code:** in the status bar, **MCP · 55561 > Connect Claude Code to this project** writes the
`unreal` entry of the project's `.mcp.json`. **Connect another client** does the same for Cursor,
VS Code, Antigravity CLI and OpenCode. Or, for yourself only:

```bash
claude mcp add --transport http unreal http://127.0.0.1:55561/mcp
```

**Any other client:** [Clients](clients.md) has the setup for Cursor, VS Code, Windsurf, Claude
Desktop, Gemini CLI and Codex. By default a client gets the [core tool list](clients.md#tool-lists):
the core tools, and four tools that search, describe and run the rest.

## 4. Ask

Name things exactly and say what "done" looks like:

> *"Create a Blueprint actor BP_Door with a door mesh and a box trigger, and open the door with a
> timeline when the player overlaps the trigger. Compile it and place one in front of the player
> start."*

The agent picks the tools. To see what it did, open **Activity** from the MCP menu in the status bar:
the last 200 calls, each with its time, duration and outcome (OK, a readback warning, failed or
refused).

## Before you let an agent loose

- **Keep the project under source control.** The agent acts with the editor's rights: it can run Python,
  save, rename and delete.
- **List vendored packs as Read Only Content Roots** in **Project Settings > Plugins > UEAgent**: no tool
  can then save, rename, move or delete anything under them. See [Settings](settings.md).
- **Ctrl+Z undoes a whole call.** Every call is one undo step named after its tool ("UEAgent:
  spawn_actor"), and an `execute_batch` is one step too. Undo restores the editor's state; a file
  the call saved stays saved until you save again.
- **A refusal is not a bug.** The call was known to crash or freeze the editor, or to be unreliable while
  Play-in-Editor runs, and its message says what to do instead. See
  [Answers and refusals](responses.md).

## Next

- [Examples](examples.md): prompts that work well, and the tools they use.
- [Skills](skills.md): install UEAgent's skills from the editor, and turn a workflow you repeat into a skill.
- [Tool explorer](explorer.md): every tool, searchable by name, domain and effect.
- [Settings](settings.md): the port, the auth token, the read-only roots and the console commands.
